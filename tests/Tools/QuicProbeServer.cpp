// QUIC 跨实现校验探针的被测端：把 QuicServer 的传输层行为按脚本暴露成 stdout 事件行，
// 对端换成独立实现（scripts/quic_cross_check.py 用 aioquic），本程序不做任何判定。
//
// 为什么要有这份工具：QUIC 服务端的端到端用例此前链接 ngtcp2 自己搭一个客户端当裁判，
// 而「同一份代码里互测」天然暴露不出双方共享的误解，还会随被测实现一起被改松。裁判换成
// 进程外的独立实现之后，合不合规范由对方的解析器说话，这里只负责把行为做出来、打成事件。
//
// 输出协议（每行一条，立即 flush，供探针与运行脚本读取）：
//   PORT <n>                     实际绑定端口
//   READY                        可以开始连接
//   CONNECTIONS <n>              在线连接数变化（「握手做不完的连接被收掉」就靠这条判）
//   STREAM <id> BYTES <n>        收到一条流数据
//   ECHOED <id> <n>              已把回显/大块排进这条流的发送队列
//   ABORTED <id> <code>          已按应用错误码收口这条流
//   DRAINING                     已发起 drain
//   STOPPED                      循环退出前的一刻
#include "Net/Quic/QuicConnection.h"
#include "Net/Quic/QuicServer.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"
#include "Platform/IO/Socket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace
{
    /**
     * @brief 命令行选项：值一律按文本收，不认识的参数直接退出
     * @details 探针脚本靠这些开关摆出各个场景，宁可拒绝启动也不要「按默认值跑出一个没人测的形态」
     */
    struct Options
    {
        std::uint16_t            port{0U};            ///< 0 表示由内核分配，实际端口打在 PORT 行
        std::string              certificateFile{};   ///< 证书 PEM
        std::string              privateKeyFile{};    ///< 私钥 PEM
        std::string              alpn{"h3"};          ///< 服务端要求的 ALPN，对端不提它就拒握手
        std::string              echoPrefix{"pong:"}; ///< 回显正文的前缀
        std::string              abortMarker{};       ///< 命中该正文即按错误码收口这条流；空即不启用
        std::uint64_t            abortCode{0x010BU};  ///< 收口用的应用错误码（RFC 9114 §8.1 的一档）
        std::string              largeReplyMarker{};  ///< 命中该正文即回一大块**不收口**的数据
        std::size_t              largeReplyBytes{2U * 1024U * 1024U};
        std::size_t              perIpLimit{0U};          ///< 单来源并发上限，0 表示不限
        long                     idleTimeoutSeconds{30};  ///< 传输层空闲超时
        long                     drainAfterRequestMs{-1}; ///< 答完第一条请求后多久收口；负数表示不
        bool                     drainImmediately{false}; ///< 启动即收口：新连接不该握手完成
        std::vector<std::string> ticketKeyFiles{};        ///< 会话票据密钥文件，可重复
    };

    void printUsage(const char *programName)
    {
        std::fprintf(stderr,
                     "用法：%s --cert <文件> --key <文件> [选项]\n"
                     "  --port <端口>                默认 0（内核分配）\n"
                     "  --alpn <名>                  服务端要求的 ALPN，默认 h3\n"
                     "  --idle-timeout <秒>          默认 30\n"
                     "  --ticket-key <文件>          可重复，跨实例共享会话票据\n"
                     "  --per-ip-limit <条数>        单来源并发上限，0 为不限\n"
                     "  --echo-prefix <文本>         默认 pong:\n"
                     "  --abort-on <文本> --abort-code <数>   命中正文即按该错误码收口这条流\n"
                     "  --large-reply-on <文本> [--large-reply-bytes <n>]  回一大块不收口的数据\n"
                     "  --drain-after-request <毫秒> 答完第一条请求后收口全部连接\n"
                     "  --drain-immediately          启动即收口\n",
                     programName);
    }

    [[noreturn]] void failWith(const std::string &message)
    {
        std::fprintf(stderr, "参数不成立：%s\n", message.c_str());
        std::exit(2);
    }

    std::string needValue(int argc, char **argv, int &index, const std::string &flag)
    {
        if (index + 1 >= argc)
        {
            failWith(flag + " 缺取值");
        }
        return argv[++index];
    }

    std::size_t parseSize(const std::string &text, const std::string &flag)
    {
        try
        {
            return static_cast<std::size_t>(std::stoull(text, nullptr, 0));
        } catch (const std::exception &)
        {
            failWith(flag + " 的取值不是非负整数：" + text);
        }
    }

    long parseLong(const std::string &text, const std::string &flag)
    {
        try
        {
            return std::stol(text, nullptr, 0);
        } catch (const std::exception &)
        {
            failWith(flag + " 的取值不是整数：" + text);
        }
    }

    Options parseArguments(const int argc, char **argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string flag = argv[index];
            if (flag == "--port")
            {
                options.port = static_cast<std::uint16_t>(parseSize(needValue(argc, argv, index, flag), flag));
            } else if (flag == "--cert")
            {
                options.certificateFile = needValue(argc, argv, index, flag);
            } else if (flag == "--key")
            {
                options.privateKeyFile = needValue(argc, argv, index, flag);
            } else if (flag == "--alpn")
            {
                options.alpn = needValue(argc, argv, index, flag);
            } else if (flag == "--idle-timeout")
            {
                options.idleTimeoutSeconds = parseLong(needValue(argc, argv, index, flag), flag);
            } else if (flag == "--ticket-key")
            {
                options.ticketKeyFiles.push_back(needValue(argc, argv, index, flag));
            } else if (flag == "--per-ip-limit")
            {
                options.perIpLimit = parseSize(needValue(argc, argv, index, flag), flag);
            } else if (flag == "--echo-prefix")
            {
                options.echoPrefix = needValue(argc, argv, index, flag);
            } else if (flag == "--abort-on")
            {
                options.abortMarker = needValue(argc, argv, index, flag);
            } else if (flag == "--abort-code")
            {
                options.abortCode = parseSize(needValue(argc, argv, index, flag), flag);
            } else if (flag == "--large-reply-on")
            {
                options.largeReplyMarker = needValue(argc, argv, index, flag);
            } else if (flag == "--large-reply-bytes")
            {
                options.largeReplyBytes = parseSize(needValue(argc, argv, index, flag), flag);
            } else if (flag == "--drain-after-request")
            {
                options.drainAfterRequestMs = parseLong(needValue(argc, argv, index, flag), flag);
            } else if (flag == "--drain-immediately")
            {
                options.drainImmediately = true;
            } else
            {
                printUsage(argv[0]);
                failWith("不认识的参数「" + flag + "」");
            }
        }
        if (options.certificateFile.empty() || options.privateKeyFile.empty())
        {
            printUsage(argv[0]);
            failWith("--cert 与 --key 都必填");
        }
        return options;
    }

    /// 打一行事件并立刻冲出去：运行脚本是边跑边看，留在缓冲里就等于没发生
    void emit(const std::string &line)
    {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }

    /**
     * @brief 常驻协程的持有者：把 Task 收进一个活到进程退出的容器
     * @details 协程帧归 Task 所有，Task 一析构，帧里「还没跑完的那一半」就没了——这里的看门狗与
     *          连接计数协程都是要一直挂着的，所以必须由一个比 main 的循环更长命的容器持有。
     *          用一张 static 表而不是栈上容器：drain 那一支是在回调里排进来的，回调不知道 main 的
     *          局部在哪，而这张表的生命周期与进程同长，不构成悬空。
     */
    class TaskKeeper
    {
    public:
        /// 启动一个协程：排进所属循环的就绪队列，帧由本表持有
        static void spawn(AsynGyanis::Core::EventLoop &loop, AsynGyanis::Core::Task<> task)
        {
            std::vector<AsynGyanis::Core::Task<>> &tasks = storage();
            tasks.push_back(std::move(task));
            loop.scheduler().schedule(tasks.back().handle());
        }

    private:
        static std::vector<AsynGyanis::Core::Task<>> &storage()
        {
            // 故意不销毁：里面是挂着的协程帧，进程退出时它们仍归本表所有（LSan 因此不会报泄漏）
            static std::vector<AsynGyanis::Core::Task<>> *const tasks = new std::vector<AsynGyanis::Core::Task<>>{};
            return *tasks;
        }
    };

    /**
     * @brief 盯在线连接数的变化并打成事件行
     * @details 「握手永远做不完的那条连接要被收掉」这类判据只在服务端内部有答案。把它打成
     *          CONNECTIONS 行，外部探针就按事件顺序核对，不必去猜「睡多久算安全」。
     * @param loop 所属事件循环（定时器用它）
     * @param server 被观察的服务端（不拥有；它比本协程长命）
     */
    AsynGyanis::Core::Task<> watchConnections(AsynGyanis::Core::EventLoop &loop, const AsynGyanis::Net::QuicServer &server)
    {
        std::size_t lastCount = 0U;
        while (true)
        {
            const std::size_t currentCount = server.connectionCount();
            if (currentCount != lastCount)
            {
                lastCount = currentCount;
                emit("CONNECTIONS " + std::to_string(currentCount));
            }
            AsynGyanis::Core::Timer tickTimer(loop);
            co_await tickTimer.waitFor(std::chrono::milliseconds{20});
        }
    }

    /**
     * @brief 等一段确定性的时长后发起 drain（非正数即立即）
     * @details 时限由调用方给：探针要判的是「对端有没有收到 CONNECTION_CLOSE」，等待由探针那边
     *          有界轮询，这里不猜对方的节奏。delayTimer 声明在协程帧内，唤醒后才排 drain 那一支。
     */
    AsynGyanis::Core::Task<> drainLater(AsynGyanis::Core::EventLoop &loop, AsynGyanis::Net::QuicServer &server, const long delayMs)
    {
        if (delayMs > 0)
        {
            AsynGyanis::Core::Timer delayTimer(loop);
            co_await delayTimer.waitFor(std::chrono::milliseconds{delayMs});
        }
        emit("DRAINING");
        TaskKeeper::spawn(loop, server.drain(std::chrono::milliseconds{0}));
        co_return;
    }

    /**
     * @brief 等 listen 协程真绑上之后宣告端口与就绪
     * @details 端口是内核分配的，绑定之前 listeningPort() 为 0；轮询到值为止是这里唯一可行的读法
     *          （直接读就是与循环线程抢同一个成员）。drain-immediately 也在这里触发，保证发生在
     *          「服务已经能接受连接」之后。
     */
    AsynGyanis::Core::Task<> announceReady(AsynGyanis::Core::EventLoop &loop, AsynGyanis::Net::QuicServer &server, const bool drainImmediately)
    {
        while (server.listeningPort() == 0U)
        {
            AsynGyanis::Core::Timer pollTimer(loop);
            co_await pollTimer.waitFor(std::chrono::milliseconds{20});
        }
        emit("PORT " + std::to_string(server.listeningPort()));
        emit("READY");
        if (drainImmediately)
        {
            co_await drainLater(loop, server, 0);
        }
        co_return;
    }
} // namespace

int main(const int argc, char **argv)
{
    const Options options = parseArguments(argc, argv);
    if (!AsynGyanis::Platform::Socket::initialize())
    {
        std::fprintf(stderr, "套接字库初始化失败\n");
        return 1;
    }

    AsynGyanis::Core::EventLoop                loop;
    AsynGyanis::Net::QuicServer::Configuration configuration;
    configuration.certificateFile       = options.certificateFile;
    configuration.privateKeyFile        = options.privateKeyFile;
    configuration.applicationProtocol   = options.alpn;
    configuration.idleTimeout           = std::chrono::seconds{options.idleTimeoutSeconds};
    configuration.sessionTicketKeyFiles = options.ticketKeyFiles;
    if (options.perIpLimit > 0U)
    {
        configuration.perIpConnectionLimiter = std::make_shared<AsynGyanis::Net::PerIpConnectionLimiter>(options.perIpLimit);
    }

    AsynGyanis::Net::QuicServer server(loop, configuration);
    // 大块正文只准备一份：每条命中探针的流都指向同一段字节，不做逐次拷贝
    const std::vector<std::uint8_t> largeReply(options.largeReplyBytes, static_cast<std::uint8_t>('x'));
    std::atomic<bool>               hasAnsweredFirstRequest{false};

    server.setStreamDataHandler(
            [&](AsynGyanis::Net::QuicConnection &connection, const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool /*isEndStream*/)
            {
                emit("STREAM " + std::to_string(streamId) + " BYTES " + std::to_string(data.size()));
                const std::string payload(data.begin(), data.end());

                if (!options.abortMarker.empty() && payload == options.abortMarker)
                {
                    connection.abortStream(streamId, options.abortCode);
                    emit("ABORTED " + std::to_string(streamId) + " " + std::to_string(options.abortCode));
                    return;
                }
                if (!options.largeReplyMarker.empty() && payload == options.largeReplyMarker)
                {
                    // 故意不收口：对端不读完就一直占着发送窗口，用来判「被流控堵住的一条流不许饿死兄弟流」
                    static_cast<void>(connection.queueStreamData(streamId, std::span<const std::uint8_t>{largeReply}, false));
                    emit("ECHOED " + std::to_string(streamId) + " " + std::to_string(largeReply.size()));
                    return;
                }
                const std::string reply = options.echoPrefix + payload;
                static_cast<void>(connection.queueStreamData(streamId, std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t *>(reply.data()), reply.size()}, true));
                emit("ECHOED " + std::to_string(streamId) + " " + std::to_string(reply.size()));
                if (!hasAnsweredFirstRequest.exchange(true) && options.drainAfterRequestMs >= 0)
                {
                    TaskKeeper::spawn(loop, drainLater(loop, server, options.drainAfterRequestMs));
                }
            });

    TaskKeeper::spawn(loop, server.listen(AsynGyanis::Core::InetAddress::resolve("127.0.0.1", options.port).value()));
    TaskKeeper::spawn(loop, watchConnections(loop, server));
    TaskKeeper::spawn(loop, announceReady(loop, server, options.drainImmediately));

    loop.run();
    emit("STOPPED");
    return 0;
}
