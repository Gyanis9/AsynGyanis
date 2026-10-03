// Http3ClientConnection 用例：在回环上向自家 HTTP/3 服务端提一条请求并收齐响应
//
// 与外部裁判（scripts/quic_outbound_cross_check.sh + aioquic）的分工：那一半判「两型实现是否一致」，
// 这一半判「我们自己两型是否还连得通」——后者跑在每个平台的 ctest 里，改坏了当场就红，
// 不必等 Linux 作业上装 aioquic 的那一步。

#include "Net/Http3/Http3ClientConnection.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
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
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::TestSupport::waitForCondition;
        using namespace std::chrono_literals;

        constexpr std::chrono::milliseconds kWaitTimeout{8000};
        /// 带大正文那两条的等待上限：见 `Http3DeliveryAttempt` 的说明，这里赌的是「丢报文之后
        /// 还要重传完」，不是「这条通路走不走得通」
        constexpr std::chrono::milliseconds kDeliveryWaitTimeout{25000};

        /// 服务端答出去的正文，两侧同一个字面串
        constexpr std::string_view kServedBody{"served-over-h3-client"};

        /// 一条比本端给每条流宣告的接收窗口（256 KiB）还大的正文：窗口归还的判据要拿它当对端。
        /// 取 300 KiB 而不是更大：这条链路上搬的是 UDP 报文，全量并行跑时搬得越多越可能丢，一丢就要
        /// 等重传；判据只要「大过一档窗口」就成立，不必多担这份风险
        constexpr std::size_t kLargeBodyByteCount = 300U * 1024U;

        /// 大正文按位置轮转 26 个字母：服务端与用例共用同一份形状，「拼回来的是不是完整那一份」才可判
        [[nodiscard]] char largeBodyByteAt(const std::size_t offset) noexcept
        {
            return static_cast<char>('a' + static_cast<int>(offset % 26U));
        }

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
             * @param configureServer 造好服务端、进入监听之前对它做的追加改动（换限额这类只能在这
             *        个窗口做：限额按连接建立那一刻交给会话）
             * @param idleTimeout 传输层的空闲收口时刻：判「对端下线之后服务端会把连接摘掉」的用例
             *        要把它调到秒级，否则归零只能等夹具默认的 30 秒
             */
            explicit RunningHttp3Server(const bool requireClientCertificates = false, const std::function<void(QuicServer &)> &configureServer = {},
                                        const std::chrono::seconds idleTimeout = std::chrono::seconds{30})
            {
                m_router.get("/probe",
                             [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 static_cast<void>(request);
                                 response.setStatus(200);
                                 response.setBody(std::string{kServedBody});
                                 response.setHeader("x-served-by", "asyngyanis-h3");
                                 // 带一条尾部字段：出站客户端的「尾字段与响应头部分表」需要一条端到端判据，
                                 // 单元级的拼接（h2 那条）证明不了 h3 这条链路上 isTrailers 真的传到位
                                 static_cast<void>(response.addTrailerField("x-checksum", "616263"));
                                 co_return;
                             });

                QuicServer::Configuration configuration;
                configuration.certificateFile = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem").string();
                configuration.privateKeyFile  = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_key.pem").string();
                configuration.idleTimeout     = idleTimeout;
                // 采集端要显式配：QuicServer 不替本服务端建一份（它的设计是与 HttpServer 共用同一端，
                // 三条通道并到一处计数）。没配时 stats() 除在线连接数外恒为零，那条请求计数判据就成了
                // 空判据——所以这里给一份自己的，而不是把判据换成更弱的
                configuration.metricsCollector = std::make_shared<HttpMetricsCollector>();
                if (requireClientCertificates)
                {
                    configuration.requireClientCertificates          = true;
                    configuration.tlsPolicy.certificateAuthorityFile = (std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem").string();
                }

                // 一条远超默认上限的正文：出站侧「越过本端胃口就只结这一条流」的判据要拿它当对端
                m_router.get("/big",
                             [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 static_cast<void>(request);
                                 response.setStatus(200);
                                 response.setBody(std::string(200U * 1024U, 'x'));
                                 co_return;
                             });

                // 收了请求但不答的静默路由：出站侧「时限到了就得把等待收回来」的判据要拿它当对端。
                // 30 秒远过用例给请求的时限，也远过服务端自己的读时限，因此答案只能由本端的时限带回来
                // 处理器按引用拿着夹具的循环：路由表是本成员，它比循环先销毁
                m_router.get("/stall",
                             [this](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 static_cast<void>(request);
                                 Core::Timer silence(m_loop);
                                 co_await silence.waitFor(std::chrono::seconds{30});
                                 response.setStatus(200);
                                 response.setBody("eventually");
                                 co_return;
                             });

                // 只沉默 200 毫秒就答的路由：给「在册连接数」那类判据留一个既能被外部线程读到非零、
                // 又不会把在途动作留到用例结束的窗口——摘除要等这条流上的处理器跑完，/stall 那 30 秒
                // 等不到（会话有未收尾的工作时，收口的连接会被推迟摘除）
                m_router.get("/brief",
                             [this](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 static_cast<void>(request);
                                 Core::Timer brief(m_loop);
                                 co_await brief.waitFor(std::chrono::milliseconds{200});
                                 response.setStatus(200);
                                 response.setBody(kServedBody);
                                 co_return;
                             });

                // 比本端单流接收窗口还大的一整份正文：出站侧「收下多少就得还多少窗口」的判据要拿它当对端
                m_router.get("/large",
                             [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 static_cast<void>(request);
                                 std::string largeBody(kLargeBodyByteCount, '\0');
                                 for (std::size_t offset = 0; offset < largeBody.size(); ++offset)
                                 {
                                     largeBody[offset] = largeBodyByteAt(offset);
                                 }
                                 response.setStatus(200);
                                 response.setBody(std::move(largeBody));
                                 co_return;
                             });

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

            /// 测试线程直接读服务端的在线连接数：这条通道本来就是给循环外的线程用的（采集端、
            /// 探活工具都这么读），因此读它的用例也必须从测试线程读，而不是绕回循环里读
            [[nodiscard]] std::size_t connectionCount() const noexcept
            {
                return m_server->connectionCount();
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
             * @param path 请求路径，默认打夹具里那条小正文的路由
             */
            Http3RequestAttempt(const std::uint16_t port, const std::string &clientCertificateFile = {}, const std::string &clientPrivateKeyFile = {},
                                const std::string path = "/probe", const std::chrono::milliseconds requestTimeout = std::chrono::milliseconds{4000}) :
                m_port(port), m_clientCertificateFile(clientCertificateFile), m_clientPrivateKeyFile(clientPrivateKeyFile)
            {
                // 路径在排协程之前落定：run() 第一次被驱动就已经在读它
                m_path           = path;
                m_requestTimeout = requestTimeout;
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

                m_response = co_await http3.request("https", "127.0.0.1:" + std::to_string(m_port), "GET", m_path, {}, {}, m_requestTimeout);
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
            /// 下面三项在构造时定死：协程帧跑在循环线程上，测试线程之后改它们没有意义也不安全
            std::string                           m_clientCertificateFile{};
            std::string                           m_clientPrivateKeyFile{};
            std::string                           m_path{};
            std::chrono::milliseconds             m_requestTimeout{4000}; ///< 这条请求自己的时限（静默对端那条要调小）
            std::chrono::steady_clock::time_point m_startedAt{};          ///< 整次尝试的起点
            std::chrono::milliseconds             m_elapsed{0};           ///< finish() 时结算的耗时
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


        /**
         * @brief 用给定的 h3 本端能力提两条请求：第一条打到超限的那条流上，第二条验连接还在
         * @details 整段跑在链路所属的循环线程上（连接对象不跨线程访问）。第二条请求是关键的一半：
         *          越界只该结掉那一条流，连接要留给别的请求用——不复位而把整条连接判死的那种实现，
         *          在这一格会红。
         */
        class BodyCapAttempt
        {
        public:
            BodyCapAttempt(const std::uint16_t port, const Http3ClientConnection::Config &config) : m_port(port), m_config(config)
            {
                m_task.emplace(run());
                m_loop.scheduler().schedule(m_task->handle());
                m_loopThread = std::thread([this] { m_loop.run(); });
            }

            ~BodyCapAttempt()
            {
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            BodyCapAttempt(const BodyCapAttempt &)            = delete;
            BodyCapAttempt &operator=(const BodyCapAttempt &) = delete;

            bool awaitFinished(const std::chrono::milliseconds timeout)
            {
                return waitForCondition([this] { return m_isFinished.load(std::memory_order_acquire); }, timeout);
            }

            [[nodiscard]] bool isConnected() const noexcept
            {
                return m_isConnected;
            }
            [[nodiscard]] const Http3ClientResponse &oversize() const noexcept
            {
                return m_oversize;
            }
            [[nodiscard]] const Http3ClientResponse &following() const noexcept
            {
                return m_following;
            }
            [[nodiscard]] bool isLinkHealthyAfterOverflow() const noexcept
            {
                return m_isHealthyAfterOverflow;
            }

        private:
            /// 在循环线程上跑完两条请求与一次健康检查
            Core::Task<> run()
            {
                const auto address = Core::InetAddress::resolve("127.0.0.1", m_port).value();
                auto       link    = std::make_shared<Http3OutboundLink>(m_loop, makeLinkConfiguration(m_port), m_config);
                m_isConnected      = co_await link->connect(address);
                if (m_isConnected)
                {
                    const std::string authority = "127.0.0.1:" + std::to_string(m_port);
                    m_oversize                  = co_await link->http3().request("https", authority, "GET", "/big", {}, {}, std::chrono::milliseconds{3000});
                    m_isHealthyAfterOverflow    = link->http3().isHealthy();
                    m_following                 = co_await link->http3().request("https", authority, "GET", "/probe", {}, {}, std::chrono::milliseconds{3000});
                }
                m_isFinished.store(true, std::memory_order_release);
                co_return;
            }

            Core::EventLoop               m_loop;
            std::uint16_t                 m_port{0U};
            Http3ClientConnection::Config m_config;
            std::optional<Core::Task<>>   m_task{};
            std::thread                   m_loopThread{};
            Http3ClientResponse           m_oversize{};
            Http3ClientResponse           m_following{};
            bool                          m_isConnected{false};
            bool                          m_isHealthyAfterOverflow{false};
            std::atomic<bool>             m_isFinished{false};
        };

        /**
         * @brief 把流号名额压到一条，问「名额用尽之后这条还算健康吗」
         * @details 整段跑在链路所属的循环线程上。第二次 request() 应当被本端直接拒掉（没有合法的
         *          新流号可提，RFC 9000 §2.1），而 isHealthy() 必须跟着说「不能再用」——池正是读它来
         *          决定这条要不要从待命表里抹掉：只看连接层还活着就会把一条只会回「换一条连接」的
         *          链路一直留在池里，每次取用都先撞一次失败。
         */
        class StreamCapAttempt
        {
        public:
            explicit StreamCapAttempt(const std::uint16_t port) : m_port(port)
            {
                m_config.maximumOpenedStreamCount = 1U;
                m_task.emplace(run());
                m_loop.scheduler().schedule(m_task->handle());
                m_loopThread = std::thread([this] { m_loop.run(); });
            }

            ~StreamCapAttempt()
            {
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            StreamCapAttempt(const StreamCapAttempt &)            = delete;
            StreamCapAttempt &operator=(const StreamCapAttempt &) = delete;

            bool awaitFinished(const std::chrono::milliseconds timeout)
            {
                return waitForCondition([this] { return m_isFinished.load(std::memory_order_acquire); }, timeout);
            }

            [[nodiscard]] bool isConnected() const noexcept
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
            [[nodiscard]] bool isHealthyAfterStreamExhaustion() const noexcept
            {
                return m_isHealthyAfterExhaustion;
            }

        private:
            /// 在循环线程上跑完两条请求与一次健康检查
            Core::Task<> run()
            {
                const std::string authority = "127.0.0.1:" + std::to_string(m_port);
                const auto        address   = Core::InetAddress::resolve("127.0.0.1", m_port).value();
                auto              link      = std::make_shared<Http3OutboundLink>(m_loop, makeLinkConfiguration(m_port), m_config);
                m_isConnected               = co_await link->connect(address);
                if (m_isConnected)
                {
                    m_first                    = co_await link->http3().request("https", authority, "GET", "/probe", {}, {}, std::chrono::milliseconds{3000});
                    m_second                   = co_await link->http3().request("https", authority, "GET", "/probe", {}, {}, std::chrono::milliseconds{3000});
                    m_isHealthyAfterExhaustion = link->http3().isHealthy();
                }
                m_isFinished.store(true, std::memory_order_release);
                co_return;
            }

            Core::EventLoop               m_loop;
            std::uint16_t                 m_port{0U};
            Http3ClientConnection::Config m_config;
            std::optional<Core::Task<>>   m_task{};
            std::thread                   m_loopThread{};
            Http3ClientResponse           m_first{};
            Http3ClientResponse           m_second{};
            bool                          m_isConnected{false};
            bool                          m_isHealthyAfterExhaustion{true};
            std::atomic<bool>             m_isFinished{false};
        };

        /**
         * @brief 在链路自己的循环线程上挂接收口跑一条 /large，并按需主动收口
         * @details 结论都由循环线程写、测试线程在停止标志置真之后才读（acquire 与写侧的 release
         *          配对），因此不构成跨线程同时读写。
         */
        class Http3DeliveryAttempt
        {
        public:
            /**
             * @brief 排好一次「握手 → 起 h3 → 带接收口提请求 → 再提一条普通请求」
             * @param port 目标端口
             * @param stopAfterBatchCount 交够这么多批就返回 false 主动收口；0 表示交完整个流
             * @note 两次请求各给 15 秒、整趟等 25 秒：这条链路上搬的是几百 KiB 的 UDP 报文，回环在负载下
             *       会丢报文，一次丢包就是几轮 PTO 退避。判据本身与快慢无关（多次交付由窗口算术保证），
             *       所以时限只要长到「真没走通」而不是「走得慢」才报红——实测整跑一轮 270 ms 上下
             */
            Http3DeliveryAttempt(const std::uint16_t port, const std::size_t stopAfterBatchCount) : m_port(port), m_stopAfterBatchCount(stopAfterBatchCount)
            {
                m_task.emplace(run());
                m_loop.scheduler().schedule(m_task->handle());
                m_loopThread = std::thread([this] { m_loop.run(); });
            }

            ~Http3DeliveryAttempt()
            {
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            Http3DeliveryAttempt(const Http3DeliveryAttempt &)            = delete;
            Http3DeliveryAttempt &operator=(const Http3DeliveryAttempt &) = delete;

            /// 等到这条链路上的两次请求都落了账
            [[nodiscard]] bool awaitFinished(const std::chrono::milliseconds timeout)
            {
                return waitForCondition([this] { return m_isFinished.load(std::memory_order_acquire); }, timeout);
            }

            [[nodiscard]] const std::vector<std::string> &batches() const noexcept
            {
                return m_batches;
            }
            [[nodiscard]] const std::vector<bool> &lastFlags() const noexcept
            {
                return m_lastBatchFlags;
            }
            [[nodiscard]] const Http3ClientResponse &first() const noexcept
            {
                return m_first;
            }
            [[nodiscard]] const Http3ClientResponse &followUp() const noexcept
            {
                return m_followUp;
            }
            [[nodiscard]] bool isLinkHealthyAfterRun() const noexcept
            {
                return m_isHealthyAfterRun;
            }
            [[nodiscard]] std::size_t inFlightAfterFollowUp() const noexcept
            {
                return m_inFlightAfterFollowUp;
            }
            /// 跑到了哪一步：整趟挂住时用它指认哪一问没回来。原子量，因为挂住时循环线程还在写它
            [[nodiscard]] const char *stage() const noexcept
            {
                return m_stage.load(std::memory_order_acquire);
            }

        private:
            Core::Task<> run()
            {
                m_stage.store("connecting", std::memory_order_release);
                const std::string authority = "127.0.0.1:" + std::to_string(m_port);
                const auto        address   = Core::InetAddress::resolve("127.0.0.1", m_port).value();
                auto              link      = std::make_shared<Http3OutboundLink>(m_loop, makeLinkConfiguration(m_port));
                if (!co_await link->connect(address))
                {
                    m_isFinished.store(true, std::memory_order_release);
                    co_return;
                }

                const Http3ResponseBodyReceiver receiver = [this](const Http3ClientResponse &, const std::string_view batch, const bool isLastBatch) -> Core::Task<bool>
                {
                    m_batches.emplace_back(batch);
                    m_lastBatchFlags.push_back(isLastBatch);
                    co_return m_stopAfterBatchCount == 0U || m_batches.size() < m_stopAfterBatchCount;
                };
                m_stage.store("first-request", std::memory_order_release);
                m_first = co_await link->http3().request("https", authority, "GET", "/large", {}, {}, std::chrono::milliseconds{15000}, receiver);
                // 收口之后同一条链路还要能接着服务：h3 的正文是分流的，结掉这一条就够
                m_isHealthyAfterRun = link->isHealthy();
                m_stage.store("follow-up", std::memory_order_release);
                m_followUp = co_await link->http3().request("https", authority, "GET", "/probe", {}, {}, std::chrono::milliseconds{15000});
                // 在途数要回得到 0：本端结掉一条流之后还有晚到的字节，那一段若被当成「一条新的在途请求」
                // 立账，就再也没有人来摘它，链路会一直看着被人用着
                m_inFlightAfterFollowUp = link->inFlightStreamCount();
                m_stage.store("done", std::memory_order_release);
                m_isFinished.store(true, std::memory_order_release);
                co_return;
            }

            Core::EventLoop             m_loop;
            std::uint16_t               m_port{0U};
            std::size_t                 m_stopAfterBatchCount{0U};
            std::optional<Core::Task<>> m_task{};
            std::thread                 m_loopThread{};
            std::vector<std::string>    m_batches{};
            std::vector<bool>           m_lastBatchFlags{};
            Http3ClientResponse         m_first{};
            Http3ClientResponse         m_followUp{};
            bool                        m_isHealthyAfterRun{false};
            std::size_t                 m_inFlightAfterFollowUp{0U};
            std::atomic<bool>           m_isFinished{false};
            std::atomic<const char *>   m_stage{"init"}; ///< 进展到哪一步（指字符串字面量，不持有内存）
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

        // 尾字段走的是正文之后那一段（RFC 9114 §4.3）：本端要把它落在 trailers 里，而不是与响应头部
        // 混成一张表——混表时这条 x-checksum 看着就像一条普通头部，消费方读不出「这是收完正文才知道的结果」
        const auto &trailers = attempt.response().trailers;
        ASSERT_EQ(trailers.size(), 1U) << "尾部字段没端到端交回，或混进了别处：" << attempt.response().headers.size();
        EXPECT_EQ(trailers[0].first, "x-checksum");
        EXPECT_EQ(trailers[0].second, "616263");
        EXPECT_FALSE(std::any_of(headers.begin(), headers.end(), [](const std::pair<std::string, std::string> &field) { return field.first == "x-checksum"; }))
                << "尾字段漏进了响应头部那张表";
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


    /**
     * @brief 响应正文越过本端上限：只结这一条流，连接留给后面的请求
     * @details 上限是使用方的胃口而不是对端犯了协议错，所以处置只能是「这一条不要了」而不能把整条
     *          连接判死——同一条连接上别的请求还要用。不复位的话对端会一直往一条我们不再读的流上发
     *          字节，本端的接收窗口额度也要等连接收口才还得回去，因此这里同时验三件事：越界那次拿到
     *          的是失败结论（且**不交半份正文**）、链路仍然健康、紧随其后的正常请求答得出来。
     * @note 证伪：把 `noteBodyBytes` 里的上限判定摘掉 → 第一条断言红（拿到 200 与 200 KiB 正文）；
     *       把越界处置改成「连连接一起判死」（不调 `abortStream` 而调 `close()`）→ 后两条红。
     */
    TEST(Http3ClientConnection, CapsResponseBodyAtTheConfiguredLimitAndKeepsTheConnection)
    {
        RunningHttp3Server server;
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        Http3ClientConnection::Config config;
        config.maximumResponseBodyBytes = 4096U;

        BodyCapAttempt attempt{server.listeningPort(), config};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "越界这条请求既没失败也没跑完，挂在那里";
        ASSERT_TRUE(attempt.isConnected()) << "链路没握上手，后面的判据都是空的";

        EXPECT_FALSE(attempt.oversize().isOk()) << "正文越过本端上限还被判成成功：" << attempt.oversize().statusCode;
        EXPECT_TRUE(attempt.oversize().body.empty()) << "越界的响应把半份正文交回了调用方";
        EXPECT_NE(attempt.oversize().errorMessage.find("maximumResponseBodyBytes"), std::string::npos) << "失败原因没指出该调哪一项：" << attempt.oversize().errorMessage;
        EXPECT_TRUE(attempt.isLinkHealthyAfterOverflow()) << "越界把整条连接判死了：同连接上别的请求跟着遭殃";
        EXPECT_TRUE(attempt.following().isOk()) << "越界之后同一条连接上的下一条请求没答上来：" << attempt.following().errorMessage;
        EXPECT_EQ(attempt.following().body, std::string{kServedBody});
    }

    /**
     * @brief 钉住：流号用尽之后这条链路不再算健康
     * @details 客户端流号严格递增，名额用尽后本端只能回「换一条连接重来」（RFC 9000 §2.1）。出站池
     *          读的正是 `isHealthy()` 来决定这条要不要从待命表里抹掉：只看连接层还活着，就会把一条
     *          只会拒绝请求的链路一直留着，每次取用都先撞一次失败，而调用方看到的只是莫名变慢。
     * @note 证伪：把 `isHealthy()` 里那条流号余量判定摘掉 → `isHealthyAfterStreamExhaustion()` 为真，
     *       本用例红。第一条请求照旧答得出来是这条用例的前提（名额确实是被它用完的，不是一开始就没）。
     */
    TEST(Http3ClientConnection, StopsReportingHealthyOnceTheStreamBudgetIsExhausted)
    {
        RunningHttp3Server server;
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        StreamCapAttempt attempt{server.listeningPort()};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "这两条请求既没跑完也没失败，挂在那里";
        ASSERT_TRUE(attempt.isConnected()) << "链路没握上手，后面的判据都是空的";

        EXPECT_TRUE(attempt.first().isOk()) << "第一条请求就该用完那个唯一的名额：" << attempt.first().errorMessage;
        EXPECT_EQ(attempt.first().body, std::string{kServedBody});
        EXPECT_FALSE(attempt.second().isOk()) << "流号到顶还被当成能提请求";
        EXPECT_NE(attempt.second().errorMessage.find("上限"), std::string::npos) << "失败原因没点出是名额用尽：" << attempt.second().errorMessage;
        EXPECT_FALSE(attempt.isHealthyAfterStreamExhaustion()) << "只会拒请求的链路还算健康：出站池会一直把它当可复用的存货";
    }

    /**
     * @brief 钉住：比本端单流接收窗口还大的响应正文能整个收下来
     * @details 本端给每条流入站流宣告 256 KiB 窗口（`QuicConnection` 的 kInitialMaximumStreamData），
     *          而协议层把 DATA 载荷的额度归还留给接收方（`Http3Connection::creditConsumedBytes` 明确
     *          把这一截扣掉，非载荷字节才就地还）。出站侧收下就是拷进了 `response.body`，因此归还点
     *          只能落在 `noteBodyBytes`：不还，服务端写到窗口边缘就再也没法推进，本端只能等到时限把
     *          整条连接掐掉——症状是「小响应全通、大响应全 timeout」。
     * @note 证伪：摘掉 `noteBodyBytes` 里那句 `extendReceiveWindow` → 服务端推满 256 KiB 后停住，
     *       本用例在 4 秒时限后拿到「响应没收齐」而红。
     */
    TEST(Http3ClientConnection, CreditsTheReceiveWindowForBodyBeyondTheAdvertisedStreamWindow)
    {
        RunningHttp3Server server;
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        Http3RequestAttempt attempt{server.listeningPort(), {}, {}, "/large"};
        ASSERT_TRUE(attempt.awaitFinished(kWaitTimeout)) << "这条大正文请求既没成也没败，挂在那里";
        ASSERT_TRUE(attempt.response().isOk()) << "大正文没整个收下：" << attempt.response().errorMessage << "（实收 " << attempt.response().body.size()
                                               << " 字节，窗口是 256 KiB）";
        ASSERT_EQ(attempt.response().body.size(), kLargeBodyByteCount) << "正文长度与服务端答出去的那一份不等";
        // 逐字节比对形状：只判长度时「攒够了但拼错位」也能绿，而窗口归还正是按累计字节数还的
        std::size_t firstMismatchOffset = kLargeBodyByteCount;
        for (std::size_t offset = 0; offset < kLargeBodyByteCount; ++offset)
        {
            if (attempt.response().body[offset] != largeBodyByteAt(offset))
            {
                firstMismatchOffset = offset;
                break;
            }
        }
        EXPECT_EQ(firstMismatchOffset, kLargeBodyByteCount) << "从第 " << firstMismatchOffset << " 字节起与服务端所答不一致";
    }

    /**
     * @brief 钉住：挂着接收口时 h3 的响应正文一批一批交出去
     * @details 判据为什么不会靠调度运气：本端给每条流的接收额度按消耗归还，而对端要把一帧（拆帧之后
     *          16 KiB）发完才轮得到下一帧，交付又排在每一轮推动通路之前——三百 KiB 的正文必然落成多次
     *          交付。拼接起来逐字节等于服务端答出去的那一份，才算既没交重也没漏交。
     * @note 证伪：把 `noteBodyBytes` 里「挂了接收口就攒着待交」改回直接进 body 并当场还额度 → 本条红
     *       （响应里留着整份正文、批数为 1）；只把 `deliverReceivedBody` 里的额度归还摘掉 → 也红
     *       （攒满一档 256 KiB 的流窗口之后对端不再发，本条等到时限）。
     */
    TEST(Http3ClientConnection, DeliversResponseBodyInBatchesAndCreditsAfterEachDelivery)
    {
        RunningHttp3Server server;
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        Http3DeliveryAttempt attempt{server.listeningPort(), 0U};
        ASSERT_TRUE(attempt.awaitFinished(kDeliveryWaitTimeout)) << "这条带接收口的请求既没成也没败，挂在「" << attempt.stage() << "」这一步";
        ASSERT_TRUE(attempt.first().isOk()) << "大正文没整个收下：" << attempt.first().errorMessage;
        EXPECT_TRUE(attempt.first().body.empty()) << "挂了接收口还把整份正文留在响应里：白攒一份内存";
        ASSERT_FALSE(attempt.batches().empty()) << "一批都没交出去";
        EXPECT_GT(attempt.batches().size(), 1U) << "只在收齐之后交了一次：逐批交付没生效";
        std::string stitched;
        for (const std::string &batch: attempt.batches())
        {
            stitched += batch;
        }
        ASSERT_EQ(stitched.size(), kLargeBodyByteCount) << "交出去的总量与对端答组成的一份不等";
        std::size_t firstMismatchOffset = kLargeBodyByteCount;
        for (std::size_t offset = 0; offset < kLargeBodyByteCount; ++offset)
        {
            if (stitched[offset] != largeBodyByteAt(offset))
            {
                firstMismatchOffset = offset;
                break;
            }
        }
        EXPECT_EQ(firstMismatchOffset, kLargeBodyByteCount) << "从第 " << firstMismatchOffset << " 字节起与对端所答不一致";
        ASSERT_EQ(attempt.lastFlags().size(), attempt.batches().size());
        EXPECT_TRUE(attempt.lastFlags().back()) << "最后一批没带上收尾标记：接收口分不清「走完了」与「断了」";
        EXPECT_EQ(std::count(attempt.lastFlags().begin(), attempt.lastFlags().end(), true), 1) << "收尾标记出现了不止一次";
        EXPECT_TRUE(attempt.isLinkHealthyAfterRun());
    }

    /**
     * @brief 钉住：h3 的接收口主动收口只结这一条流，连接留着还能再提请求
     * @details 与 h2 那一支同形，也是比 HTTP/1.1 强的地方：h1 半路收口只能关掉整条连接。这里连三条
     *          一起判：交够指定的批数就停、拿到的仍是完整头部且算成功、紧随其后的普通请求在同一链路
     *          上答得出来。最后一问还顺手盯着「在途数回不回得到 0」——本端结掉一条流之后仍有晚到的
     *          字节，若那条账被重立起来就没人再来摘，链路会一直看着被人占用。
     * @note 证伪：把 `noteBodyBytes` 里「不认这条流就只还额度」换回 `exchangeFor`（顺手立账）→ 在途数
     *       那条红（实测确实有晚到的字节）；摘掉收口那支的交付判据（`deliverReceivedBody` 返回假的
     *       true）→ 批数那条红。至于本端结流那一句 `abortStream`：它的效果全在对端一侧（别再为这条
     *       流发字节），自家服务端这几条路由并不阻塞，摘与不摘本层看不出来——那套动作本身在传输层已有
     *       直测（`TestQuicStreamLayer` 的 STOP_SENDING 一组）。
     */
    TEST(Http3ClientConnection, StopsDeliveringAndKeepsTheConnectionForTheNextRequest)
    {
        RunningHttp3Server server;
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        Http3DeliveryAttempt attempt{server.listeningPort(), 2U};
        ASSERT_TRUE(attempt.awaitFinished(kDeliveryWaitTimeout)) << "这条收口的请求既没成也没败，挂在「" << attempt.stage() << "」这一步";
        EXPECT_EQ(attempt.batches().size(), 2U) << "接收口说的「不要了」没被当数";
        EXPECT_TRUE(attempt.first().isOk()) << "主动收口是一次成功的交换：" << attempt.first().errorMessage;
        EXPECT_TRUE(attempt.first().body.empty());
        EXPECT_TRUE(attempt.isLinkHealthyAfterRun()) << "收掉一条流把整条链路也判死了";
        EXPECT_TRUE(attempt.followUp().isOk()) << "收口之后同一条链路上的下一条请求没答上来：" << attempt.followUp().errorMessage;
        EXPECT_EQ(attempt.followUp().body, std::string{kServedBody});
        EXPECT_EQ(attempt.inFlightAfterFollowUp(), 0U) << "晚到的字节给已收口的流重立了账：在途数再也回不到 0";
    }

    /**
     * @brief 钉住：对端收了请求不答话时，本端的时限必须能把等待收回来
     * @details 这条量的不是「等不等得到答案」，而是「等不到答案时这条请求还算不算数」：出站 h3 的
     *          等待挂在「读到下一条报文」上，对端握住请求不答就没有答案可等。两条判据各指一处：
     *          结论得是失败而不是成功（半路放弃不能报 200）；整趟耗时得贴着时限而不是贴着等待上限
     *          （否则就是「碰巧又来了一条报文」，不是时限起作用）。此前这条通路对「请求时限到点」
     *          零直测。
     * @note 证伪：摘掉 `request()` 里那把 `DeadlineGuard` → 本条挂在 awaitFinished 上红（一秒二的时限，
     *       等满 8 秒也没收场）。
     */
    TEST(Http3ClientConnection, ReclaimsTheRequestWhenTheDeadlinePassesWhileThePeerIsSilent)
    {
        RunningHttp3Server server;
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";

        Http3RequestAttempt attempt{server.listeningPort(), {}, {}, "/stall", std::chrono::milliseconds{1200}};
        ASSERT_TRUE(attempt.awaitFinished(std::chrono::seconds{8})) << "时限早就掐断了，这条请求还没收场：等待没被收回来";
        EXPECT_FALSE(attempt.response().isOk()) << "对端一个字都没答，本端不该报成功：" << attempt.response().errorMessage;
        EXPECT_LT(attempt.elapsed(), std::chrono::seconds{5}) << "收场用了 " << attempt.elapsed().count() << " 毫秒，不像时限起作用的样子";
    }

    /**
     * @brief 在线连接数必须能被循环外的线程读到，且增删两侧都反映
     * @details 采集端与探活工具都在外部线程上读这个数，而连接表只归事件循环线程：读侧不经过原子
     *          镜像就是一次数据竞争（一边读红黑树的计数，一边收包路径摘连接写同一处）。
     *          这里两侧各等一次条件成立，不赌「线程恰好重叠」：漏刷登记侧第一条等待就超时，
     *          漏刷摘除侧第二条超时。
     */
    TEST(Http3ClientConnection, ReportsTheOnlineConnectionCountToOtherThreadsOnBothSides)
    {
        // 空闲收口给到一秒档：客户端那条请求收场后就不再发包，服务端只能靠空闲超时判它收口。
        // 留夹具默认的 30 秒，「归零」就落在等待预算之外，等于把判据交给调度
        RunningHttp3Server server{false, {}, std::chrono::seconds{1}};
        ASSERT_NE(server.listeningPort(), 0U) << "服务端没进入监听，后面的判据都是空的";
        EXPECT_EQ(server.connectionCount(), 0U) << "还没人来连就该是零，否则增长侧读到的数认不出是谁加的";

        // /brief 只沉默 200 毫秒：这段就是在册窗口，够外部线程按毫秒级节拍读到一次非零；它一定会答完，
        // 流上不留在途动作，摘除侧随后能真的把这条摘掉（用 /stall 的话处理器要等 30 秒，摘除被推迟）
        Http3RequestAttempt attempt{server.listeningPort(), {}, {}, "/brief", std::chrono::milliseconds{4000}};
        ASSERT_TRUE(waitForCondition([&server] { return server.connectionCount() > 0U; }, std::chrono::seconds{5}))
                << "一次真实握手之后外部线程读不到在册连接，说明登记侧没把计数刷上去";
        ASSERT_TRUE(attempt.awaitFinished(std::chrono::seconds{5})) << "那条请求没在时限内收场，后面的归零判据就是空的";

        // 客户端在协程末尾自己收口；服务端要么在最后一个报文里见到收口，要么在一秒的空闲超时后
        // 自己判死，而清扫节拍是 10 毫秒一档，因此 8 秒预算内必然归零
        ASSERT_TRUE(waitForCondition([&server] { return server.connectionCount() == 0U; }, kWaitTimeout))
                << "对端下线之后读数没归零，说明摘除侧漏了减一，那个数会一直虚高到进程结束";
    }

} // namespace AsynGyanis::Net
