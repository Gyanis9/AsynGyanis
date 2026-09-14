/** @file TestTcpClient.cpp 出站 TCP 客户端：连回本地 HTTP 服务器，读响应确认 200 */
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
        Core::Task<void> doClientWork(Core::EventLoop &loop, uint16_t port,
                                      bool &outOk, std::string &outResponse)
        {
            std::unique_ptr<TcpStream> streamPtr = co_await TcpClient::connect(loop, "127.0.0.1", port);
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

        auto work = doClientWork(clientLoop, port, ok, responseText);
        if (!work.isReady())
            clientLoop.scheduler().schedule(work.handle());
        clientLoop.run();

        ASSERT_TRUE(ok) << "没有连上服务器";
        EXPECT_TRUE(responseText.find("200") != std::string::npos ||
                    responseText.find("served-hello") != std::string::npos)
                << "响应：" << responseText;
    }
} // namespace AsynGyanis::Net