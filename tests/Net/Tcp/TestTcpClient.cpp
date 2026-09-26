// 出站 TCP 客户端：连回本地 HTTP 服务器，读响应确认 200
#include "HttpTestSupport.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Tcp/TcpClient.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;
        constexpr auto kTimeout = std::chrono::seconds{10};

        /// 连接到服务器、发 GET 请求、读响应，结果写进 outOk 与 outResponse
        /// host 按值收：协程帧里存的是引用的话，调用方递字面量时那个临时 std::string 在跨过
        /// 第一次 co_await 之前就没了（ASan 报 stack-use-after-scope）
        Core::Task<void> doClientWork(Core::EventLoop &loop, std::string host, uint16_t port,
                                      bool &outOk, std::string &outResponse)
        {
            std::unique_ptr<TcpStream> streamPtr = co_await TcpClient::connect(loop, std::move(host), port);
            if (!streamPtr) { outOk = false; loop.stop(); co_return; }
            outOk = true;
            TcpStream &stream = *streamPtr;
            const std::string request = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            co_await stream.writeAll(request.data(), request.size());
            char buffer[256];
            while (true)
            {
                const ssize_t n = co_await stream.read(buffer, sizeof(buffer));
                if (n <= 0) break;
                outResponse.append(buffer, static_cast<std::size_t>(n));
            }
            loop.stop();
        }
    }

    TEST(TcpClient, ConnectsToLocalhostAndReadsResponse)
    {
        auto fixture = std::make_unique<RunningHttpServerFixture>(
                HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture->awaitRunning(kTimeout));
        const uint16_t port = fixture->listeningPort();
        ASSERT_NE(port, 0U);

        Core::EventLoop clientLoop;
        bool            ok = false;
        std::string     responseText;

        auto work = doClientWork(clientLoop, "127.0.0.1", port, ok, responseText);
        if (!work.isReady())
            clientLoop.scheduler().schedule(work.handle());
        clientLoop.run();

        ASSERT_TRUE(ok) << "没有连上服务器";
        EXPECT_TRUE(responseText.find("200") != std::string::npos ||
                    responseText.find("served-hello") != std::string::npos)
                << "响应：" << responseText;
    }

    /**
     * @brief 起一台只监听给定地址的服务器；地址不可用时返回空
     * @details 「空」只表示这台机器绑不上那个地址（例如没有 IPv6 回环），不表示被测代码有问题——
     *          调用方据此跳过，而不是报一条与环境无关的红
     * @param listenAddress 监听地址，端口 0 由内核分配
     * @return std::unique_ptr<RunningHttpServerFixture> 起好了交出夹具，起不来交空
     */
    namespace
    {
        /**
         * @brief 自建一条客户端循环，向 host:port 发一条 GET 并读完响应
         * @param host 目标主机文本（IPv6 就写 "::1"，解析与建套接字都按它自己的族走）
         * @param port 目标端口
         * @param outOk 是否连上并读完
         * @param outResponse 读回的响应文本
         */
        void driveClientWork(const std::string &host, const std::uint16_t port, bool &outOk, std::string &outResponse)
        {
            Core::EventLoop clientLoop;
            auto            work = doClientWork(clientLoop, host, port, outOk, outResponse);
            if (!work.isReady())
            {
                clientLoop.scheduler().schedule(work.handle());
            }
            clientLoop.run();
        }
    } // namespace

    /**
     * @brief 钉住：解析出 IPv6 候选时，套接字按**那条地址自己的协议族**建
     * @details AsyncSocket::create 的默认档是 AF_INET，拿它去 connect 一个 sockaddr_in6 只会以
     *          「协议族不符」收场——修之前纯 IPv6 目标是**根本连不上**，双栈目标却看不出来
     *          （解析器把 IPv4 排在前面，第一条就成了）。所以这里让服务器只监听 ::1：唯一那条路
     *          就是 IPv6。这台机器没有可用的 IPv6 回环时按环境跳过。
     */
    TEST(TcpClient, ConnectsToIpv6LoopbackAndReadsResponse)
    {
        const auto fixture = tryStartHttpServerOn(Core::InetAddress{0, "::1"});
        if (fixture == nullptr)
        {
            GTEST_SKIP() << "::1 绑不上：这台机器没有可用的 IPv6 回环";
        }
        ASSERT_TRUE(fixture->awaitRunning(kTimeout));
        const std::uint16_t port = fixture->listeningPort();
        ASSERT_NE(port, 0U);

        bool        ok = false;
        std::string responseText;
        driveClientWork("::1", port, ok, responseText);

        ASSERT_TRUE(ok) << "连不上只监听 ::1 的服务器：IPv6 候选被拿去用 IPv4 的套接字连了";
        EXPECT_NE(responseText.find("served-hello"), std::string::npos) << "响应：" << responseText;
    }
} // namespace AsynGyanis::Net