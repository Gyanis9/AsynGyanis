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
            /**
             * @brief 起一台服务端
             * @param requireClientCertificates 为真时打开双向 TLS：要求并校验客户端证书，信任锚就用
             *        下面那张自签夹具（它既是服务端身份又是它自己的根，因此同一份文件两头通用）
             */
            explicit RunningHttp3Server(const bool requireClientCertificates = false)
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
                if (requireClientCertificates)
                {
                    configuration.requireClientCertificates          = true;
                    configuration.tlsPolicy.certificateAuthorityFile = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem").string();
                }

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
            /**
             * @brief 排好一次「握手 → 起 h3 → 提一条请求」并起循环线程
             * @param port 目标端口
             * @param clientCertificateFile 客户端身份证书；与私钥同时给才出示（双向 TLS 那一侧）
             * @param clientPrivateKeyFile 配套的私钥
             */
            Http3RequestAttempt(const std::uint16_t port, const std::string &clientCertificateFile = {}, const std::string &clientPrivateKeyFile = {}) :
                m_port(port), m_clientCertificateFile(clientCertificateFile), m_clientPrivateKeyFile(clientPrivateKeyFile)
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

            /// 整次尝试（握手 + 起 h3 + 等响应）的耗时：判「是被立刻收掉还是等满自己的时限」用它
            [[nodiscard]] std::chrono::milliseconds elapsed() const noexcept
            {
                return m_elapsed;
            }

        private:
            Core::Task<> run()
            {
                QuicClientConnection::Configuration configuration;
                configuration.hostName                           = "127.0.0.1";
                configuration.applicationProtocolIdentifiers     = {std::string{"h3"}};
                configuration.tlsPolicy.certificateAuthorityFile = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem").string();
                configuration.handshakeTimeout                   = std::chrono::milliseconds{4000};
                // 身份给不给，就是双向 TLS 那两条用例之间唯一的差别（服务端与信任锚两边一致）
                configuration.clientCertificateFile = m_clientCertificateFile;
                configuration.clientPrivateKeyFile  = m_clientPrivateKeyFile;

                auto client              = std::make_unique<QuicClientConnection>(m_loop, std::move(configuration));
                m_startedAt              = std::chrono::steady_clock::now();
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
                finish();
                co_return;
            }

            void finish()
            {
                m_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_startedAt);
                m_isFinished.store(true, std::memory_order_release);
            }

            Core::EventLoop m_loop;
            std::uint16_t   m_port{0U};
            /// 下面两项在构造时定死：协程帧跑在循环线程上，测试线程之后改它没有意义也不安全
            std::string                           m_clientCertificateFile{};
            std::string                           m_clientPrivateKeyFile{};
            std::chrono::steady_clock::time_point m_startedAt{}; ///< 整次尝试的起点
            std::chrono::milliseconds             m_elapsed{0};  ///< finish() 时结算的耗时
            std::optional<Core::Task<>>           m_task{};
            std::thread                           m_loopThread{};
            Http3ClientResponse                   m_response{};
            bool                                  m_isStarted{false};
            std::atomic<bool>                     m_isFinished{false};
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


    /**
     * @brief 双向 TLS 开着时，出示受信任证书的客户端照样拿得到服务
     * @details 判据与第一条同源（状态码 + 正文 + 那条 `x-served-by` + 服务端的请求计数）：加了 mTLS
     *          之后这四个还要全成立，才说明「要求客户端证书」没有把正常通路一起带走。
     */
    TEST(Http3ClientConnection, ServesARequestOverMutualTlsWhenTheClientPresentsATrustedCertificate)
    {
        RunningHttp3Server server{true};
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        Http3RequestAttempt attempt{server.listeningPort(), (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem").string(),
                                    (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_key.pem").string()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "带身份的请求既没成也没败";
        EXPECT_TRUE(attempt.isStarted()) << "h3 层的三条本端单向流没开出来";
        EXPECT_TRUE(attempt.response().isOk()) << "合法客户端证书没被接受：" << attempt.response().errorMessage;
        EXPECT_EQ(attempt.response().statusCode, 200);
        EXPECT_EQ(attempt.response().body, std::string{kServedBody});
        EXPECT_EQ(server.servedRequestCount(), 1U) << "服务端没数到这条请求：本端的结论是自己拼的";
    }

    /**
     * @brief 服务端要求客户端证书时，不带身份的客户端拿不到任何服务
     * @details 判据落在**服务端平面**。TLS 1.3 里客户端收完服务端的 Finished 就自认握手完成，它对
     *          「服务端后来有没有接受我的 Certificate」没有任何可见性（拒绝只能靠加密后的 alert 传达），
     *          所以「客户端那边握手成功」不能拿来当 mTLS 的判据——实现完全正确时那条断言也会红。
     *          这里看两件：请求没拿到答，以及**服务端的请求计数一条也不涨**，后者才是「没被服务」的
     *          证据而不是本端自己的推断。
     * @note 已知未覆盖：这条被拒的连接什么时候从服务端连接表里散去。现测到的是服务端的 TLS 既不
     *       Failed 也不 Completed（OpenSSL 在没有可用客户端证书时不让握手结论），本端靠请求时限收手；
     *       它是否要干等到 idleTimeout 才散，得先量准再单独钉一条用例。
     */
    TEST(Http3ClientConnection, DoesNotServeAClientWithoutCertificateWhenTheServerRequiresOne)
    {
        RunningHttp3Server server{true};
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        // 与正面那条唯一的差别就是没有客户端身份：服务端、信任锚、请求内容全都一样
        Http3RequestAttempt attempt{server.listeningPort()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "不带身份的请求既没回也没按时限收场，挂在那里";
        EXPECT_FALSE(attempt.response().isOk()) << "服务端要求客户端证书，不带身份的客户端却拿到了响应：mTLS 没生效";
        EXPECT_EQ(server.servedRequestCount(), 0U) << "服务端把这条请求当成一条请求处理了：它根本没被要求出示证书";
        // 这条被拒的连接不是攥在服务端手里等本端时限：实测读数 46 毫秒（请求时限是 4 秒）。把它钉成判据的
        // 理由是——「不能认证的对端每次都能挂住服务端一份 TLS 会话整整一个空闲窗口」那种形态会在此刻红
        EXPECT_LT(attempt.elapsed(), std::chrono::seconds{2}) << "没在时限内被服务端收掉，而是本端等满了请求预算：" << attempt.elapsed().count() << " 毫秒";
    }
    /**
     * @brief 出示一张不在服务端信任锚里的客户端证书，同样拿不到服务
     * @details 上一条钉的是「不给证书就不服务」，这一条钉的是「给的证书真的被拿去查信任锚了」：少了它，
     *          「要求出示证书、但出示什么都不查」那种实现可以两条都绿。夹具换成 CN=asyngyanis-test 那张
     *          ——与自签的 test_ip_cert 无关，但自身与私钥配对成立，因此失败原因只能落在信任上。
     */
    TEST(Http3ClientConnection, DoesNotServeAClientPresentingAnUntrustedCertificate)
    {
        RunningHttp3Server server{true};
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        Http3RequestAttempt attempt{server.listeningPort(), (std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem").string(),
                                    (std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem").string()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "被拒的请求既没回也没按时限收场，挂在那里";
        EXPECT_FALSE(attempt.response().isOk()) << "信任锚之外的客户端证书被接受了：" << attempt.response().errorMessage;
        EXPECT_EQ(server.servedRequestCount(), 0U) << "服务端查都没查就把这条请求处理了：CA 那一项根本没被用上";
    }

} // namespace AsynGyanis::Net
