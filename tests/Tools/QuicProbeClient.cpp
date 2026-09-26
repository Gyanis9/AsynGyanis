// QUIC 出站跨实现校验探针的被测端：把 QuicClientConnection 的传输行为打成 stdout 事件行，
// 对端换成独立实现（scripts/quic_outbound_cross_check.py 用 aioquic 起服务端并回显），
// 本程序不做任何判定。
//
// 为什么要有这份工具：出站方向此前没有任何独立实现碰过——树内用例的对面是本框架自己的服务端，
// 「同一份代码里互测」暴露不出双方共享的误解（角色化时最容易留这一类：两型都按同一套错误直觉走，
// 互相验得出握手，换成真实现就解不开）。裁判换成进程外的独立实现之后，合不合规范由对方的解析器说话。
//
// 输出协议（每行一条，立即 flush，供运行脚本核对）：
//   CONNECTED <alpn>          握手完成与协商到的应用层协议
//   STREAM <id> SENT <n>      在这样一条双向流上送出去 n 字节
//   ECHOED <id> <n>           从同一条流收回 n 字节（内容与送出的逐字相同才打这行）
//   FAILED <原因>             没握手成功、对端不回、或回显内容不符
//   CLOSED                    本端已收口，探针即将退出
#include "Net/Quic/QuicClientConnection.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/IO/Socket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // 本文件在全局作用域里写，故按名取用（与 QuicProbeServer.cpp 同一口径）
    using AsynGyanis::Core::EventLoop;
    using AsynGyanis::Core::InetAddress;
    using AsynGyanis::Core::Task;
    using AsynGyanis::Net::QuicClientConnection;

    /**
     * @brief 命令行选项：值一律按文本收，缺参数或参数不合法即拒绝启动
     * @details 探针脚本靠这些开关摆出各个场景；宁可退出码非 0 也不要「按默认值跑出一个没人测的形态」
     */
    struct Options
    {
        std::uint16_t port{0U};                       ///< 对端端口，必填
        std::string   hostName{};                     ///< SNI 与证书校验目标，必填
        std::string   certificateAuthority{};         ///< 信任锚 PEM，必填
        std::string   applicationProtocol{"h3"};      ///< 要提供的 ALPN
        std::string   message{"quic-outbound-probe"}; ///< 送出去并期待原样回显的正文
        long          handshakeTimeoutMs{3000};       ///< 握手时限
        long          waitTimeoutMs{4000};            ///< 等回显的上限
        std::string   address{"127.0.0.1"};           ///< 对端地址
    };

    /**
     * @brief 打一行事件并立刻冲刷
     * @param line 事件文本
     */
    void emitLine(const std::string &line)
    {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }

    /**
     * @brief 把文本转成正整数
     * @param text 命令行原文
     * @param fieldName 出错时报给用户的字段名
     * @return long 转换结果
     */
    long parseNumber(const std::string &text, const char *fieldName)
    {
        errno            = 0;
        char      *end   = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (errno != 0 || end == nullptr || *end != '\0' || text.empty())
        {
            std::fprintf(stderr, "参数 %s 的值不是十进制整数：%s\n", fieldName, text.c_str());
            std::exit(2);
        }
        return value;
    }

    /**
     * @brief 解析命令行
     * @param argumentCount 实参个数
     * @param argumentValues 实参表
     * @return Options 解析结果
     */
    Options parseOptions(const int argumentCount, char *argumentValues[])
    {
        Options options;
        for (int argumentIndex = 1; argumentIndex < argumentCount; ++argumentIndex)
        {
            const std::string flag{argumentValues[argumentIndex]};
            const auto        nextValue = [&]
            {
                if (argumentIndex + 1 >= argumentCount)
                {
                    std::fprintf(stderr, "参数 %s 缺少取值\n", flag.c_str());
                    std::exit(2);
                }
                return std::string{argumentValues[++argumentIndex]};
            };
            if (flag == "--port")
            {
                options.port = static_cast<std::uint16_t>(parseNumber(nextValue(), "port"));
            } else if (flag == "--host")
            {
                options.hostName = nextValue();
            } else if (flag == "--ca")
            {
                options.certificateAuthority = nextValue();
            } else if (flag == "--alpn")
            {
                options.applicationProtocol = nextValue();
            } else if (flag == "--message")
            {
                options.message = nextValue();
            } else if (flag == "--address")
            {
                options.address = nextValue();
            } else if (flag == "--handshake-timeout")
            {
                options.handshakeTimeoutMs = parseNumber(nextValue(), "handshake-timeout");
            } else if (flag == "--wait-timeout")
            {
                options.waitTimeoutMs = parseNumber(nextValue(), "wait-timeout");
            } else
            {
                std::fprintf(stderr, "不认识的参数：%s\n", flag.c_str());
                std::exit(2);
            }
        }
        if (options.port == 0U || options.hostName.empty() || options.certificateAuthority.empty())
        {
            std::fprintf(stderr, "缺少必填参数：--port / --host / --ca 三项都要给\n");
            std::exit(2);
        }
        return options;
    }

    /**
     * @brief 在客户端循环线程上跑完「握手 → 开流 → 送 → 等回显 → 收口」一整趟
     * @param loop 客户端所属循环
     * @param options 命令行选项
     * @param isFinished 结果已就位的标志（测试脚本线程等它）
     * @param isSuccess 整趟是否走通
     */
    Task<> runProbe(EventLoop &loop, const Options &options, std::atomic<bool> &isFinished, std::atomic<bool> &isSuccess)
    {
        QuicClientConnection::Configuration configuration;
        configuration.hostName                           = options.hostName;
        configuration.applicationProtocolIdentifiers     = {options.applicationProtocol};
        configuration.tlsPolicy.certificateAuthorityFile = options.certificateAuthority;
        configuration.handshakeTimeout                   = std::chrono::milliseconds{options.handshakeTimeoutMs};

        auto       client        = std::make_unique<QuicClientConnection>(loop, configuration);
        const auto serverAddress = InetAddress::resolve(options.address, options.port);
        if (!serverAddress.has_value())
        {
            emitLine("FAILED 地址解析失败");
            client.reset();
            isSuccess.store(false);
            isFinished.store(true);
            co_return;
        }

        if (!co_await client->connect(*serverAddress))
        {
            emitLine("FAILED 握手未完成");
            client.reset();
            isSuccess.store(false);
            isFinished.store(true);
            co_return;
        }
        emitLine("CONNECTED " + client->negotiatedApplicationProtocol());

        const std::int64_t streamId = client->openStream();
        if (streamId < 0)
        {
            emitLine("FAILED 开不出双向流（对端给的双向流额度为 0？）");
            client.reset();
            isSuccess.store(false);
            isFinished.store(true);
            co_return;
        }
        const std::span<const std::uint8_t> outbound{reinterpret_cast<const std::uint8_t *>(options.message.data()), options.message.size()};
        const std::size_t                   acceptedByteCount = client->writeStream(streamId, outbound, true);
        emitLine("STREAM " + std::to_string(streamId) + " SENT " + std::to_string(acceptedByteCount));

        // 等回显：一条条收，最多收到上限为止。没有后台协程，正是 pumpOnce 的用法本意
        std::vector<std::uint8_t> echoed{};
        const auto                deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{options.waitTimeoutMs};
        while (echoed.size() < options.message.size() && std::chrono::steady_clock::now() < deadline)
        {
            co_await client->pumpOnce();
            for (auto received = client->takeReceivedData(streamId); !received.empty(); received = client->takeReceivedData(streamId))
            {
                echoed.insert(echoed.end(), received.begin(), received.end());
                if (echoed.size() >= options.message.size())
                {
                    break;
                }
            }
        }

        if (echoed.size() != options.message.size() || std::memcmp(echoed.data(), options.message.data(), echoed.size()) != 0)
        {
            emitLine("FAILED 回显不符（收到 " + std::to_string(echoed.size()) + " 字节）");
            client.reset();
            isSuccess.store(false);
            isFinished.store(true);
            co_return;
        }
        emitLine("ECHOED " + std::to_string(streamId) + " " + std::to_string(echoed.size()));

        client->close();
        co_await client->pumpOnce();
        emitLine("CLOSED");
        client.reset();
        isSuccess.store(true);
        isFinished.store(true);
        co_return;
    }
} // namespace

int main(const int argumentCount, char *argumentValues[])
{
    const Options options = parseOptions(argumentCount, argumentValues);

    AsynGyanis::Core::EventLoop loop;
    std::atomic<bool>           isFinished{false};
    std::atomic<bool>           isSuccess{false};
    auto                        task = runProbe(loop, options, isFinished, isSuccess);
    // 协程要由循环线程首启：这里只排进就绪队列，与用例里那套夹具同一接法
    loop.scheduler().schedule(task.handle());
    std::thread loopThread{[&loop] { loop.run(); }};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{options.handshakeTimeoutMs + options.waitTimeoutMs + 4000};
    while (!isFinished.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (!isFinished.load())
    {
        emitLine("FAILED 探针整体超时");
    }

    loop.stop();
    loopThread.join();
    const bool didSucceed = isFinished.load() && isSuccess.load();
    emitLine(didSucceed ? "RESULT ok" : "RESULT failed");
    std::fflush(stdout);
    return didSucceed ? 0 : 1;
}
