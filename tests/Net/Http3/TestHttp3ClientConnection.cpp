// Http3ClientConnection 用例：在回环上向自家 HTTP/3 服务端提一条请求并收齐响应
//
// 与外部裁判（scripts/quic_outbound_cross_check.sh + aioquic）的分工：那一半判「两型实现是否一致」，
// 这一半判「我们自己两型是否还连得通」——后者跑在每个平台的 ctest 里，改坏了当场就红，
// 不必等 Linux 作业上装 aioquic 的那一步。

#include "Net/Http3/Http3ClientConnection.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Router.h"
#include "Net/Quic/QuicClientConnection.h"
#include "Net/Quic/QuicServer.h"
#include "Platform/IO/Socket.h"

#include "CommonTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::TestSupport::waitForCondition;
        using namespace std::chrono_literals;

        constexpr std::chrono::milliseconds kWaitTimeout{8000};

        /// 服务端答出去的正文，两侧同一个字面串
        constexpr std::string_view kServedBody{"served-over-h3-client"};

        /**
         * @brief 起一个真的 HTTP/3 服务端：带一条 GET 路由，跑在自己的循环线程上
         * @details 与传输层那一份夹具的差别只在这里挂了 Router——本文件判的是「请求进得来、答得出去」
         */
        class RunningHttp3Server
        {
        public:
            RunningHttp3Server()
            {
                m_router.get("/probe",
                             [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 static_cast<void>(request);
                                 response.setStatus(200);
                                 response.setBody(std::string{kServedBody});
                                 response.setHeader("x-served-by", "asyngyanis-h3");
                                 co_return;
                             });

                QuicServer::Configuration configuration;
                configuration.certificateFile = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem").string();
                configuration.privateKeyFile  = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_key.pem").string();
                configuration.idleTimeout     = std::chrono::seconds{30};
                // 采集端要显式配：QuicServer 不替本服务端建一份（它的设计是与 HttpServer 共用同一端，
                // 三条通道并到一处计数）。没配时 stats() 除在线连接数外恒为零，那条请求计数判据就成了
                // 空判据——所以这里给一份自己的，而不是把判据换成更弱的
                configuration.metricsCollector = std::make_shared<HttpMetricsCollector>();

                m_server = std::make_unique<QuicServer>(m_loop, configuration);
                m_server->setRouter(m_router);
                m_listenTask.emplace(m_server->listen(Core::InetAddress::resolve("127.0.0.1", 0).value()));
                m_loop.scheduler().schedule(m_listenTask->handle());
                m_loopThread = std::thread([this] { m_loop.run(); });
                static_cast<void>(waitForCondition([this] { return m_server->listeningPort() != 0U; }, kWaitTimeout));
            }

            ~RunningHttp3Server()
            {
                m_server->stop();
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            RunningHttp3Server(const RunningHttp3Server &) = delete;

            RunningHttp3Server &operator=(const RunningHttp3Server &) = delete;

            [[nodiscard]] std::uint16_t listeningPort() const noexcept
            {
                return m_server->listeningPort();
            }

            [[nodiscard]] std::size_t servedRequestCount() const noexcept
            {
                return m_server->stats().totalRequestCount;
            }

        private:
            Core::EventLoop             m_loop;
            Router                      m_router;
            std::unique_ptr<QuicServer> m_server{};
            std::optional<Core::Task<>> m_listenTask{};
            std::thread                 m_loopThread{};
        };

        /**
         * @brief 在客户端自己的循环线程上跑完「握手 → 起 h3 → 提一条请求」
         */
        class Http3RequestAttempt
        {
        public:
            explicit Http3RequestAttempt(const std::uint16_t port) : m_port(port)
            {
                m_task.emplace(run());
                m_loop.scheduler().schedule(m_task->handle());
                m_loopThread = std::thread([this] { m_loop.run(); });
            }

            ~Http3RequestAttempt()
            {
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            Http3RequestAttempt(const Http3RequestAttempt &) = delete;

            Http3RequestAttempt &operator=(const Http3RequestAttempt &) = delete;

            bool awaitFinished(const std::chrono::milliseconds timeout)
            {
                return waitForCondition([this] { return m_isFinished.load(std::memory_order_acquire); }, timeout);
            }

            [[nodiscard]] const Http3ClientResponse &response() const noexcept
            {
                return m_response;
            }

            [[nodiscard]] bool isStarted() const noexcept
            {
                return m_isStarted;
            }

        private:
            Core::Task<> run()
            {
                QuicClientConnection::Configuration configuration;
                configuration.hostName                           = "127.0.0.1";
                configuration.applicationProtocolIdentifiers     = {std::string{"h3"}};
                configuration.tlsPolicy.certificateAuthorityFile = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem").string();
                configuration.handshakeTimeout                   = std::chrono::milliseconds{4000};

                auto       client        = std::make_unique<QuicClientConnection>(m_loop, std::move(configuration));
                const auto serverAddress = Core::InetAddress::resolve("127.0.0.1", m_port).value();
                if (!co_await client->connect(serverAddress))
                {
                    m_response.errorMessage = "握手没成";
                    finish();
                    co_return;
                }

                Http3ClientConnection http3{*client};
                m_isStarted = co_await http3.start();
                if (!m_isStarted)
                {
                    m_response.errorMessage = "h3 层起不来";
                    finish();
                    co_return;
                }

                m_response = co_await http3.request("https", "127.0.0.1:" + std::to_string(m_port), "GET", "/probe", {}, {}, std::chrono::milliseconds{4000});
                co_await http3.shutdown();
                client.reset();
                m_isFinished.store(true, std::memory_order_release);
                co_return;
            }

            void finish()
            {
                m_isFinished.store(true, std::memory_order_release);
            }

            Core::EventLoop             m_loop;
            std::uint16_t               m_port{0U};
            std::optional<Core::Task<>> m_task{};
            std::thread                 m_loopThread{};
            Http3ClientResponse         m_response{};
            bool                        m_isStarted{false};
            std::atomic<bool>           m_isFinished{false};
        };
    } // namespace

    /**
     * @brief 出站 h3 客户端与自家服务端在回环上走通一个完整往返
     * @details 判据要成对才不空心：状态码与正文（答出来了）、那条 `x-served-by` 头部（头段解全了）、
     *          服务端的请求计数（服务端真把这条请求当成一条请求处理了，而不是本端自己拼出来的结论）。
     *          跨实现的那一半判据在 aioquic 裁判里，这里只保证自家两型不断。
     */
    TEST(Http3ClientConnection, CompletesARequestRoundTripAgainstOurOwnServer)
    {
        RunningHttp3Server server;
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        Http3RequestAttempt attempt{server.listeningPort()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "这次请求既没成也没败";
        EXPECT_TRUE(attempt.isStarted()) << "h3 层的三条本端单向流没开出来";
        EXPECT_EQ(attempt.response().statusCode, 200) << "原因：" << attempt.response().errorMessage;
        EXPECT_EQ(attempt.response().body, kServedBody) << "正文与路由所答不一致";
        const auto &headers = attempt.response().headers;
        EXPECT_TRUE(std::any_of(headers.begin(), headers.end(),
                                [](const std::pair<std::string, std::string> &field) { return field.first == "x-served-by" && field.second == "asyngyanis-h3"; }))
                << "响应头段里没找到路由带出的那一项";
        EXPECT_TRUE(attempt.response().isOk()) << "结论不自洽：" << attempt.response().errorMessage;
        EXPECT_EQ(server.servedRequestCount(), 1U) << "服务端没数到这条请求：本端的结论是自己拼的";
    }


} // namespace AsynGyanis::Net
