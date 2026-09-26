// Http3ClientConnection 用例：在回环上向自家 HTTP/3 服务端提一条请求并收齐响应
//
// 与外部裁判（scripts/quic_outbound_cross_check.sh + aioquic）的分工：那一半判「两型实现是否一致」，
// 这一半判「我们自己两型是否还连得通」——后者跑在每个平台的 ctest 里，改坏了当场就红，
// 不必等 Linux 作业上装 aioquic 的那一步。

#include "Net/Http3/Http3ClientConnection.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
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
            /**
             * @brief 起一台服务端
             * @param requireClientCertificates 为真时打开双向 TLS：要求并校验客户端证书，信任锚就用
             *        下面那张自签夹具（它既是服务端身份又是它自己的根，因此同一份文件两头通用）
             * @param configureServer 造好服务端、进入监听之前对它做的追加改动（换限额这类只能在这
             *        个窗口做：限额按连接建立那一刻交给会话）
             */
            explicit RunningHttp3Server(const bool requireClientCertificates = false, const std::function<void(QuicServer &)> &configureServer = {})
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
                if (configureServer)
                {
                    configureServer(*m_server);
                }
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

        /**
         * @brief 造一份指向回环某端口的出站 QUIC 配置（与 `Http3RequestAttempt` 用同一套信任锚）
         * @param port 目标端口（当前只用于日志，配置本身按主机名校验）
         * @return QuicClientConnection::Configuration 配好的配置
         */
        QuicClientConnection::Configuration makeLinkConfiguration(const std::uint16_t port)
        {
            QuicClientConnection::Configuration configuration;
            configuration.hostName                           = "127.0.0.1";
            configuration.applicationProtocolIdentifiers     = {std::string{"h3"}};
            configuration.tlsPolicy.certificateAuthorityFile = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem").string();
            configuration.handshakeTimeout                   = std::chrono::milliseconds{4000};
            configuration.idleTimeout                        = std::chrono::seconds{30};
            static_cast<void>(port);
            return configuration;
        }

        /**
         * @brief 在链路自己的循环线程上把「出站池 + h3 链路」的几条契约一次跑完
         * @details 池的取用、判死与计数都只能在连接所属的循环上读：一条 h3 链路连着 QUIC 连接与 UDP
         *          套接字，跨线程看它的健康状态就是踩别人正在写的内存（本框架的循环与连接对象不跨线程
         *          访问）。所以整段判据放在一个协程里跑完，结果交回测试线程读；链路也在协程结束前放手，
         *          销毁同样留在循环线程上。
         */
        class PoolRoundTrip
        {
        public:
            explicit PoolRoundTrip(const std::uint16_t port) : m_port(port)
            {
                m_task.emplace(run());
                m_loop.scheduler().schedule(m_task->handle());
                m_loopThread = std::thread([this] { m_loop.run(); });
            }

            ~PoolRoundTrip()
            {
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            PoolRoundTrip(const PoolRoundTrip &)            = delete;
            PoolRoundTrip &operator=(const PoolRoundTrip &) = delete;

            bool awaitFinished(const std::chrono::milliseconds timeout)
            {
                return waitForCondition([this] { return m_isFinished.load(std::memory_order_acquire); }, timeout);
            }

            [[nodiscard]] bool isConnected() const noexcept
            {
                return m_isConnected;
            }
            [[nodiscard]] bool isReusedSameLink() const noexcept
            {
                return m_isReusedSameLink;
            }
            [[nodiscard]] bool isAcquireAfterCloseNull() const noexcept
            {
                return m_isAcquireAfterCloseNull;
            }
            [[nodiscard]] std::size_t linkCountAfterAdopt() const noexcept
            {
                return m_linkCountAfterAdopt;
            }
            [[nodiscard]] std::size_t linkCountAfterSecondAdopt() const noexcept
            {
                return m_linkCountAfterSecondAdopt;
            }
            [[nodiscard]] std::size_t inFlightAfterRequest() const noexcept
            {
                return m_inFlightAfterRequest;
            }
            [[nodiscard]] bool isFirstLinkAfterSecondAdopt() const noexcept
            {
                return m_isFirstLinkAfterSecondAdopt;
            }
            [[nodiscard]] std::size_t linkCountAfterEvict() const noexcept
            {
                return m_linkCountAfterEvict;
            }
            [[nodiscard]] const Http3ClientResponse &response() const noexcept
            {
                return m_response;
            }

        private:
            /// 在循环线程上跑完整段：建链 → 进池 → 取回 → 提一条请求 → 再建一条 → 关掉之后取
            Core::Task<> run()
            {
                const HttpOutboundEndpointKey key{std::string{"127.0.0.1"}, m_port, true};
                const auto                    address = Core::InetAddress::resolve("127.0.0.1", m_port).value();

                auto first    = std::make_shared<Http3OutboundLink>(m_loop, makeLinkConfiguration(m_port));
                m_isConnected = co_await first->connect(address);
                if (!m_isConnected)
                {
                    finish();
                    co_return;
                }

                m_pool.adoptHttp3(key, first);
                m_isReusedSameLink    = (m_pool.acquireHttp3(key) == first);
                m_linkCountAfterAdopt = m_pool.idleHttp3LinkCount();

                m_response = co_await first->http3().request("https", "127.0.0.1:" + std::to_string(m_port), "GET", "/probe", {}, {}, std::chrono::milliseconds{4000});
                // 请求收完之后在途就该归零：还留着数说明流上的账没结清
                m_inFlightAfterRequest = m_pool.http3MaximumInFlightStreamCount();

                // 一台主机只留一条：第二条链路应当被拒收
                auto second = std::make_shared<Http3OutboundLink>(m_loop, makeLinkConfiguration(m_port));
                if (co_await second->connect(address))
                {
                    m_pool.adoptHttp3(key, second);
                }
                m_linkCountAfterSecondAdopt = m_pool.idleHttp3LinkCount();
                // 只数条数判不出「覆盖」与「保留第一条」的差别（两种都剩一条）：这里比的是身份
                m_isFirstLinkAfterSecondAdopt = (m_pool.acquireHttp3(key) == first);

                // 关掉之后取用要判死并抹掉：QPACK 动态表与拥塞状态都随连接作废，留着它只会交出废链
                first->http3().close();
                m_isAcquireAfterCloseNull = (m_pool.acquireHttp3(key) == nullptr);
                m_linkCountAfterEvict     = m_pool.idleHttp3LinkCount();

                m_pool.closeAll();
                first.reset();
                second.reset();
                finish();
                co_return;
            }

            void finish()
            {
                m_isFinished.store(true, std::memory_order_release);
            }

            Core::EventLoop             m_loop;
            std::uint16_t               m_port{0U};
            HttpOutboundConnectionPool  m_pool;                           ///< 被测对象：池按 h2 那一对的形状管 h3 链路
            std::optional<Core::Task<>> m_task{};                         ///< 跑判据的协程
            std::thread                 m_loopThread{};                   ///< 链路所属的循环线程
            Http3ClientResponse         m_response{};                     ///< 经链路提的那条请求的结论
            bool                        m_isConnected{false};             ///< 链路是否握上手并起好 h3 层
            bool                        m_isReusedSameLink{false};        ///< 取回来的是不是放进去那一条
            bool                        m_isAcquireAfterCloseNull{false}; ///< 关掉之后再取应当拿空
            std::size_t                 m_linkCountAfterAdopt{0U};        ///< 进池后的链路条数
            std::size_t                 m_linkCountAfterSecondAdopt{0U};
            bool                        m_isFirstLinkAfterSecondAdopt{false}; ///< 再塞第二条之后的链路条数（应仍为 1）
            std::size_t                 m_inFlightAfterRequest{0U};           ///< 请求收完之后的最大在途流数（应为 0）
            std::size_t                 m_linkCountAfterEvict{0U};            ///< 判死之后的链路条数（应为 0）
            std::atomic<bool>           m_isFinished{false};                  ///< 结果已就位
        };


        /**
         * @brief 在同一条链路上连提两条请求，看服务端在单连接请求数到量之后怎么处置第二条
         * @details 整段跑在链路所属的循环线程上（连接对象不跨线程访问），两条结论交回测试线程读
         */
        class TwoRequestAttempt
        {
        public:
            explicit TwoRequestAttempt(const std::uint16_t port) : m_port(port)
            {
                m_task.emplace(run());
                m_loop.scheduler().schedule(m_task->handle());
                m_loopThread = std::thread([this] { m_loop.run(); });
            }

            ~TwoRequestAttempt()
            {
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            TwoRequestAttempt(const TwoRequestAttempt &)            = delete;
            TwoRequestAttempt &operator=(const TwoRequestAttempt &) = delete;

            bool awaitFinished(const std::chrono::milliseconds timeout)
            {
                return waitForCondition([this] { return m_isFinished.load(std::memory_order_acquire); }, timeout);
            }

            [[nodiscard]] bool isLinkConnected() const noexcept
            {
                return m_isConnected;
            }
            [[nodiscard]] const Http3ClientResponse &first() const noexcept
            {
                return m_first;
            }
            [[nodiscard]] const Http3ClientResponse &second() const noexcept
            {
                return m_second;
            }

        private:
            /// 在循环线程上跑完两条请求：第一条应当正常应答，第二条落在「本端已经不再受理」那一侧
            Core::Task<> run()
            {
                const auto address = Core::InetAddress::resolve("127.0.0.1", m_port).value();
                auto       link    = std::make_shared<Http3OutboundLink>(m_loop, makeLinkConfiguration(m_port));
                m_isConnected      = co_await link->connect(address);
                if (m_isConnected)
                {
                    m_first  = co_await link->http3().request("https", "127.0.0.1:" + std::to_string(m_port), "GET", "/probe", {}, {}, std::chrono::milliseconds{3000});
                    m_second = co_await link->http3().request("https", "127.0.0.1:" + std::to_string(m_port), "GET", "/probe", {}, {}, std::chrono::milliseconds{3000});
                }
                m_isFinished.store(true, std::memory_order_release);
                co_return;
            }

            Core::EventLoop             m_loop;
            std::uint16_t               m_port{0U};
            std::optional<Core::Task<>> m_task{};
            std::thread                 m_loopThread{};
            Http3ClientResponse         m_first{};
            Http3ClientResponse         m_second{};
            bool                        m_isConnected{false};
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


    /**
     * @brief 出站池按与 h2 同一形状管 h3 链路：取回同一条、一台主机只留一条、判死即抹掉
     * @details `Http3OutboundLink` 存在的理由是**共同持有**——h3 会话只借用它的 QUIC 连接，池与调用方
     *          各拿一半就会松开一条还在用的链路。这里连池的三条契约一起判：取回的就是放进去那条
     *          （复用发生在流上，不摘走）、第二条链路不占位置、关掉之后取既拿到空也把表清干净。
     * @note 证伪：把 `adoptHttp3` 的「已有货就不收」去掉 → 第二条链路那条判据红；把 `acquireHttp3`
     *       里的健康判定删掉 → 最后两条判据一起红（把废链交回调用方）。
     */
    TEST(Http3OutboundLink, IsPooledAndEvictedLikeTheHttp2Side)
    {
        RunningHttp3Server server;
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        PoolRoundTrip roundTrip{server.listeningPort()};
        ASSERT_TRUE(roundTrip.awaitFinished(kWaitTimeout)) << "池里这一段既没跑完也没报错，挂在那里";
        ASSERT_TRUE(roundTrip.isConnected()) << "链路没握上手，后面的判据都是空的";
        EXPECT_TRUE(roundTrip.response().isOk()) << "经链路提的请求没拿到答：" << roundTrip.response().errorMessage;
        EXPECT_EQ(roundTrip.response().body, std::string{kServedBody});

        EXPECT_TRUE(roundTrip.isReusedSameLink()) << "取回来的不是放进去那条：h3 的复用没接上";
        EXPECT_EQ(roundTrip.linkCountAfterAdopt(), 1U);
        EXPECT_EQ(roundTrip.inFlightAfterRequest(), 0U) << "请求收完之后仍算在途：流上的账没结清";
        EXPECT_TRUE(roundTrip.isFirstLinkAfterSecondAdopt()) << "第二条把第一条顶掉了：只数条数看不出这种覆盖，被在途握着的那条不能换";
        EXPECT_EQ(roundTrip.linkCountAfterSecondAdopt(), 1U) << "同一台主机留了两条 h3 链路：多占一个 UDP 端口与一份 QPACK 状态，换不来吞吐";
        EXPECT_TRUE(roundTrip.isAcquireAfterCloseNull()) << "链路已经关掉，池还把同一条交出去";
        EXPECT_EQ(roundTrip.linkCountAfterEvict(), 0U) << "判死的链路留在表上：下一次取用会拿到一条废链";
    }


    /**
     * @brief 端到端钉住 `QuicServer::setLimits()`：新限额要真的交到此后建立的会话手上
     * @details 光验 getter 判不出接线——本条走一条真链路：服务端在 listen() 之前把单连接请求数上限
     *          换成 1，第一条请求正常应答，第二条就该落在「本端已经不再受理」那一侧（会话到量即
     *          宣告排空，后来的流不再被服务）。把会话那侧的读源改回 Configuration，本条会两问全绿，
     *          因为默认档的 1000 条上限根本到不了。
     */
    TEST(Http3ClientConnection, HonoursThePerConnectionRequestLimitSetBeforeListening)
    {
        RunningHttp3Server server{false, [](QuicServer &quicServer)
                                  {
                                      HttpServerLimits limits;
                                      limits.maximumRequestsPerConnection = 1;
                                      quicServer.setLimits(limits);
                                  }};
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        TwoRequestAttempt attempt{server.listeningPort()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "两条请求既没跑完也没报错，挂在那里";
        ASSERT_TRUE(attempt.isLinkConnected()) << "链路没握上手，后面的判据都是空的";
        EXPECT_TRUE(attempt.first().isOk()) << "第一条请求就没成：" << attempt.first().errorMessage;
        EXPECT_FALSE(attempt.second().isOk()) << "单连接请求数到量之后第二条仍被服务：setLimits 没交到会话手上";
    }

} // namespace AsynGyanis::Net
