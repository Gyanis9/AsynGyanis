// 出站客户端的 HTTP/3 通路用例：开关、复用、失败回落、一个端点只探一次、流式上传绕开 h3
//
// 判据手法：摆三种对端形态各照一种情形——只有 UDP 在听（h3 走得通）、只有 TCP 在听（只能靠回落）、
// UDP 端口被一个永不应答的套接字占住（探测只能等满时限）。每个形态各自绑一个内核挑的端口，
// 用例不要求同一端口上 TCP 与 UDP 并存：那要两次 bind 之间没人把这个号拿走，在并行跑的全量门禁里
// 是一处真实竞态（实测确实会被同批其它用例占走，症状是夹具起不来）。

#include "Net/Http/Client/HttpClient.h"

#include "HttpTestSupport.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http/HttpBodySource.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Router.h"
#include "Net/Quic/QuicServer.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/Socket.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;
        using namespace std::chrono_literals;

        constexpr std::chrono::milliseconds kWaitTimeout{10000};

        /// 既是服务端身份又是它自己的信任锚，因此同一份文件两头通用（SAN 里有 IP:127.0.0.1）
        const std::filesystem::path kLoopbackCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_cert.pem";
        const std::filesystem::path kLoopbackKeyPath         = std::filesystem::path(TEST_FIXTURES_DIR) / "test_ip_key.pem";

        /// 两条通路各答一份自己的正文：响应体就是「谁答的」的见证
        constexpr std::string_view kTcpServedBody{"served-over-tcp"};
        constexpr std::string_view kHttp3ServedBody{"served-over-h3"};

        constexpr std::string_view kProbeRoutePath = "/probe";
        constexpr std::string_view kBigRoutePath   = "/big";

        /// 大路由的正文长度：远过用例里那条小上限，又不至于让测试进程搬太多字节
        constexpr std::size_t kBigBodyByteCount = 200U * 1024U;

        /**
         * @brief 把「定正文」的 GET/POST 与一条「大正文」GET 挂到给定路由表上
         * @param router 目标路由表
         * @param servedBody 这条通路答出去的正文
         */
        void registerProbeRoutes(Router &router, std::string_view servedBody)
        {
            router.get(std::string{kProbeRoutePath},
                       [servedBody](HttpRequest &, HttpResponse &response) -> Core::Task<>
                       {
                           response.setBody(std::string{servedBody});
                           co_return;
                       });
            // POST 那一条是给「流式上传」用的：它必须是 POST 才合上传的形状，而路由表按方法分档，
            // 拿 GET 的路由接 POST 只会得到 405，那份「谁答的」的见证就没了
            router.post(std::string{kProbeRoutePath},
                        [servedBody](HttpRequest &, HttpResponse &response) -> Core::Task<>
                        {
                            response.setBody(std::string{servedBody});
                            co_return;
                        });
            router.get(std::string{kBigRoutePath},
                       [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                       {
                           response.setBody(std::string(kBigBodyByteCount, 'x'));
                           co_return;
                       });
        }

        /// 对端这一侧摆成什么形态
        enum class PeerShape
        {
            Http3Only,   ///< 只有 QUIC 在听：h3 该走通，TCP 那一路没人接
            TcpOnly,     ///< 只有 HTTPS 在听：h3 探测立刻被拒，该回落到 TCP
            UdpBlackHole ///< HTTPS 在听，且同端口的 UDP 被一个永不应答的套接字占住：探测只能等满时限
        };

        /**
         * @brief 按形态摆出一台对端，跑在自己的循环线程上
         * @details 黑洞形态先把 HTTPS 绑上（端口由内核挑）、再把同一个号占成 UDP：两个协议族各一张
         *          协议控制块，自己占自己不冲突；这样排次序也不会出现「刚挑的号被人占走」那一窗。
         */
        class RunningPeer
        {
        public:
            explicit RunningPeer(const PeerShape shape)
            {
                if (shape == PeerShape::Http3Only)
                {
                    startHttp3Server();
                } else
                {
                    startHttpServer();
                }
                m_loopThread = std::thread([this] { m_loop.run(); });
                m_isReady.store(waitForBothUp(shape), std::memory_order_release);
            }

            ~RunningPeer()
            {
                // 次序按两份既有夹具各自的口径：QUIC 侧在循环停之前收口，
                // HTTPS 侧在循环线程 join 之后 close
                if (m_quic != nullptr)
                {
                    m_quic->stop();
                }
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
                if (m_https != nullptr)
                {
                    m_https->close();
                }
            }

            RunningPeer(const RunningPeer &)            = delete;
            RunningPeer &operator=(const RunningPeer &) = delete;

            /// 客户端要打的端口号
            [[nodiscard]] std::uint16_t port() const noexcept
            {
                return m_port;
            }

            /// 对端是否已进入各自的服务循环（构造里已经等过，这里只把结论交出来）
            [[nodiscard]] bool awaitRunning() const
            {
                return m_isReady.load(std::memory_order_acquire);
            }

        private:
            /**
             * @brief 起 QUIC 服务端并把 listen 协程投进循环（端口由内核挑，落定之后才有号）
             */
            void startHttp3Server()
            {
                QuicServer::Configuration configuration;
                configuration.certificateFile = kLoopbackCertificatePath.string();
                configuration.privateKeyFile  = kLoopbackKeyPath.string();
                configuration.idleTimeout     = std::chrono::seconds{30};
                // 路由表排在服务端之前声明：setRouter 存的是引用，而成员按声明逆序销毁
                m_quicRouter.emplace();
                registerProbeRoutes(*m_quicRouter, kHttp3ServedBody);
                m_quic = std::make_unique<QuicServer>(m_loop, configuration);
                m_quic->setRouter(*m_quicRouter);
                m_quicListenTask.emplace(m_quic->listen(Core::InetAddress::localhost(0)));
                m_loop.scheduler().schedule(m_quicListenTask->handle());
            }

            /// 起 HTTPS 服务端（端口交给内核挑）并把 start 协程投进循环
            void startHttpServer()
            {
                m_https = std::make_unique<HttpsServer>(m_loop, Core::InetAddress::localhost(0), kLoopbackCertificatePath.string(), kLoopbackKeyPath.string());
                registerProbeRoutes(m_https->router(), kTcpServedBody);
                m_httpsListenTask.emplace(m_https->start());
                m_loop.scheduler().schedule(m_httpsListenTask->handle());
            }

            /**
             * @brief 占住这个 UDP 端口但永不应答：客户端的 Initial 发出去就石沉大海
             * @details 描述符持到析构，端口因此一直被占着——这正是「黑洞」与「没人听」（立刻吃一个
             *          ICMP 端口不可达）的分别，两条用例判的是两件不同的事。
             */
            void holdUdpPortSilently(const std::uint16_t port)
            {
                Platform::SocketAddress requested{};
                requested.length           = sizeof(sockaddr_in);
                auto *addressIn            = reinterpret_cast<sockaddr_in *>(&requested.storage);
                addressIn->sin_family      = AF_INET;
                addressIn->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addressIn->sin_port        = htons(port);
                m_silentUdp.emplace(Platform::DatagramSocket::bindTo(requested));
            }

            /// 等对端进入服务循环；黑洞形态还要那把 UDP 确实占住了号
            [[nodiscard]] bool waitForBothUp(const PeerShape shape)
            {
                if (shape == PeerShape::Http3Only)
                {
                    if (!waitForCondition([this] { return m_quic != nullptr && m_quic->listeningPort() != 0U; }, kWaitTimeout))
                    {
                        return false;
                    }
                    m_port = m_quic->listeningPort();
                    return m_port != 0U;
                }
                if (!waitForCondition([this] { return m_https != nullptr && m_https->isRunning(); }, kWaitTimeout))
                {
                    return false;
                }
                // 端口要等 start() 里的 bind/listen 落定之后才读得出来（见 TcpServer::runAcceptLoop）
                m_port = m_https->listeningPort();
                if (m_port == 0U)
                {
                    return false;
                }
                if (shape == PeerShape::UdpBlackHole)
                {
                    // 这个号此刻已被上面的 TCP 监听握着自己占，不会有别的进程把它顺走；
                    // 两个协议族各一张协议控制块，自己占自己不冲突
                    holdUdpPortSilently(m_port);
                    return m_silentUdp.has_value() && m_silentUdp->isValid();
                }
                return true;
            }

            /// 先声明、后放手：描述符与循环都在 Winsock 引用之前收尾
            Platform::Socket::Initialization        m_network{};
            Core::EventLoop                         m_loop;
            std::optional<Router>                   m_quicRouter{};
            std::unique_ptr<QuicServer>             m_quic{};
            std::unique_ptr<HttpsServer>            m_https{};
            std::optional<Platform::DatagramSocket> m_silentUdp{};
            std::optional<Core::Task<>>             m_httpsListenTask{};
            std::optional<Core::Task<>>             m_quicListenTask{};
            std::atomic<bool>                       m_isReady{false};
            std::uint16_t                           m_port{0};
            std::thread                             m_loopThread;
        };

        /// 一个带池的客户端走完一串请求之后的结论
        struct Http3ClientRunOutcome
        {
            std::vector<int>                       statusCodes;         ///< 每条请求的状态码；0 表示这条失败
            std::vector<std::string>               bodies;              ///< 与状态码对齐的正文，读它就是读「谁答的」
            std::vector<std::chrono::milliseconds> elapsed;             ///< 每条请求各自花了多久
            std::size_t                            http3LinkCount{0};   ///< 跑完时池里留着的 h3 链路条数
            std::size_t                            http1IdleBetween{0}; ///< 第二条请求之前（收口之后）的 h1 空闲条数
            std::size_t                            http2IdleBetween{0}; ///< 同上，h2 待命条数
        };

        /**
         * @brief 在一条客户端循环上用同一个 HttpClient 走完 urls
         * @param urls 依次发出的地址
         * @param isEnabled 本次是否打开 HTTP/3
         * @param poolConfig 池参数（要验正文上限就从这里传）
         * @param requestTimeout 每条请求的整体时限
         * @param streamedUpload 为真时把每条请求换成带 bodySource 的 POST 上传
         * @param closeIdleBetweenRequests 为真时在两条请求之间收一次口：把第二条逼回「建连」那一路。
         *        不这么做它会直接复用第一条留下的连接、走不到探测那一步，判据就成了空判据
         * @return Http3ClientRunOutcome 状态码、正文、耗时与收尾时的链路计数
         */
        Http3ClientRunOutcome runClientRequests(const std::vector<std::string> &urls, const bool isEnabled, const HttpOutboundConnectionPool::Config &poolConfig = {},
                                                const std::chrono::milliseconds requestTimeout = std::chrono::seconds{20}, const bool streamedUpload = false,
                                                const bool closeIdleBetweenRequests = false)
        {
            Core::EventLoop loop;
            Core::TlsPolicy policy;
            policy.certificateAuthorityFile = kLoopbackCertificatePath.string();
            Http3ClientRunOutcome outcome;
            // 客户端由协程在循环线程上建、也在循环线程上收：h3 链路底下是 QUIC 连接与它的注册结构，
            // 那些东西只能在所属循环上销毁（见本框架的事件循环线程契约）
            std::optional<HttpClient> client;

            auto drive = [&loop, &client, &urls, &outcome, &policy, poolConfig, isEnabled, requestTimeout, streamedUpload, closeIdleBetweenRequests]() -> Core::Task<>
            {
                client.emplace(loop, poolConfig, policy);
                client->setHttp3Enabled(isEnabled);
                for (const std::string &url: urls)
                {
                    if (closeIdleBetweenRequests && !outcome.statusCodes.empty())
                    {
                        client->closeIdleConnections();
                        outcome.http1IdleBetween = client->idleConnectionCount();
                        outcome.http2IdleBetween = client->idleHttp2ConnectionCount();
                    }
                    HttpClientRequest request;
                    if (streamedUpload)
                    {
                        request.method      = "POST";
                        request.contentType = "text/plain";
                        // 一段都不给的来源：这条用例只判「走的哪条通路」，正文形状由来源本身另有用例钉
                        request.bodySource = []() -> Core::Task<std::optional<std::string>> { co_return std::nullopt; };
                    }
                    const auto                                started  = std::chrono::steady_clock::now();
                    const std::unique_ptr<HttpClientResponse> response = co_await client->send(url, request, requestTimeout);
                    outcome.elapsed.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started));
                    outcome.statusCodes.push_back(response ? response->statusCode : 0);
                    outcome.bodies.push_back(response ? response->body : std::string{});
                }
                outcome.http3LinkCount = client->idleHttp3LinkCount();
                client.reset();
                loop.stop();
                co_return;
            };
            // 闭包先落到具名对象上再调用：惰性协程的帧记的是闭包地址，临时量在语句结束就析构
            auto work = drive();
            if (!work.isReady())
            {
                loop.scheduler().schedule(work.handle());
            }
            loop.run();
            return outcome;
        }

        std::string probeUrl(const std::uint16_t port)
        {
            return "https://127.0.0.1:" + std::to_string(port) + std::string{kProbeRoutePath};
        }

        std::string bigUrl(const std::uint16_t port)
        {
            return "https://127.0.0.1:" + std::to_string(port) + std::string{kBigRoutePath};
        }
    } // namespace

    /**
     * @brief 钉住开关的两侧：开着才走 h3，关着绝不碰 h3
     * @details 对端只有 UDP 在听，于是「走了哪条通路」不必看内部计数：走 h3 就答得出来（200 + h3
     *          正文），走 TCP 就连不上（状态码 0）。判据写坏成「开关没用、恒走 h3」时，关着那一跑
     *          也会拿到 200，当场红。
     */
    TEST(HttpClientHttp3, SwitchDecidesWhetherOutboundUsesHttp3)
    {
        ASSERT_TRUE(std::filesystem::exists(kLoopbackCertificatePath)) << "缺少证书夹具：" << kLoopbackCertificatePath.string();
        RunningPeer peer(PeerShape::Http3Only);
        ASSERT_TRUE(peer.awaitRunning()) << "QUIC 对端没起来，后面的判据都是空的";
        const std::string url = probeUrl(peer.port());

        const Http3ClientRunOutcome enabled = runClientRequests({url}, true);
        ASSERT_EQ(enabled.statusCodes.size(), 1U);
        EXPECT_EQ(enabled.statusCodes[0], 200) << "开着 h3 而对端在听 UDP，这条却没能走 h3";
        EXPECT_EQ(enabled.bodies[0], kHttp3ServedBody);
        EXPECT_EQ(enabled.http3LinkCount, 1U) << "h3 链路没进池：下次请求还得重做一遍握手";

        const Http3ClientRunOutcome disabled = runClientRequests({url}, false);
        ASSERT_EQ(disabled.statusCodes.size(), 1U);
        EXPECT_EQ(disabled.statusCodes[0], 0) << "开关关着却走了 h3：默认档必须与既有行为逐字一致";
        EXPECT_EQ(disabled.http3LinkCount, 0U);
    }

    /**
     * @brief 钉住复用的本义：同主机的连续请求共用一条 h3 链路
     * @details 三条请求都从 h3 那侧拿到答话、跑完只剩一条链路。少任何一侧都不算证明：只看响应会漏掉
     *          「每条各握一条 QUIC、旧的悄悄关掉」，只看条数会漏掉「请求其实根本没走 h3」。
     */
    TEST(HttpClientHttp3, ReusesOneLinkAcrossSequentialRequests)
    {
        RunningPeer peer(PeerShape::Http3Only);
        ASSERT_TRUE(peer.awaitRunning());
        const std::string           url     = probeUrl(peer.port());
        const Http3ClientRunOutcome outcome = runClientRequests({url, url, url}, true);
        ASSERT_EQ(outcome.statusCodes.size(), 3U);
        for (std::size_t index = 0; index < outcome.statusCodes.size(); ++index)
        {
            EXPECT_EQ(outcome.statusCodes[index], 200) << "第 " << index + 1 << " 条没走通";
            EXPECT_EQ(outcome.bodies[index], kHttp3ServedBody) << "第 " << index + 1 << " 条不是 h3 答的";
        }
        EXPECT_EQ(outcome.http3LinkCount, 1U) << "三条请求留下了不止一条链路：复用没发生";
    }

    /**
     * @brief 钉住失败回落：对端不听 UDP 时，开了 h3 的客户端仍从 TCP 那一路拿到 200
     * @details 回落在这一档是必需的而不是可选的：h3 不通就把整条请求判失败，等于「一个开关拧掉了
     *          一台本来好好的主机」。这里 UDP 端口根本没人占，连接立刻被拒，探测不花时限。
     */
    TEST(HttpClientHttp3, FallsBackToTcpWhenPeerHasNoHttp3)
    {
        RunningPeer peer(PeerShape::TcpOnly);
        ASSERT_TRUE(peer.awaitRunning());

        const Http3ClientRunOutcome outcome = runClientRequests({probeUrl(peer.port())}, true);
        ASSERT_EQ(outcome.statusCodes.size(), 1U);
        EXPECT_EQ(outcome.statusCodes[0], 200) << "h3 不通就整个失败：没兑现「失败回落 TCP」这条承诺";
        EXPECT_EQ(outcome.bodies[0], kTcpServedBody) << "答话的不是 TCP 那侧";
        EXPECT_EQ(outcome.http3LinkCount, 0U) << "走不通的 h3 不该留在池里";
    }

    /**
     * @brief 钉住「一个端点只探一次」：UDP 被黑洞时，探测时限只由第一条请求付
     * @details 让 UDP 端口被一个永不应答的套接字占住，探测只能等满时限（三秒）才交回，于是两条
     *          请求的耗时差就是「第二条有没有再探一遍」的直接读数。阈值两侧都留了一倍以上余量。
     * @details 第二条之前先收一次口，否则它直接复用第一条留下的连接、根本走不到建连那一步——
     *          那样的用例摘掉记账也照样绿。收口之后两条计数为 0 是本例的前提，所以也钉住：
     *          将来 ALPN 改成默认走 h2 时这里会先红，提醒重写而不是静默失效。
     */
    TEST(HttpClientHttp3, ProbesHttp3OnlyOncePerEndpoint)
    {
        RunningPeer peer(PeerShape::UdpBlackHole);
        ASSERT_TRUE(peer.awaitRunning()) << "黑洞形态的对端没起来（HTTPS 没进循环，或 UDP 端口没占住）";
        const std::string url = probeUrl(peer.port());

        const Http3ClientRunOutcome outcome = runClientRequests({url, url}, true, {}, std::chrono::seconds{30}, false, true);
        ASSERT_EQ(outcome.statusCodes.size(), 2U);
        EXPECT_EQ(outcome.statusCodes[0], 200) << "探测吃掉时限之后，TCP 那侧仍该答话";
        EXPECT_EQ(outcome.statusCodes[1], 200);
        EXPECT_EQ(outcome.bodies[0], kTcpServedBody);
        EXPECT_EQ(outcome.bodies[1], kTcpServedBody);
        EXPECT_EQ(outcome.http3LinkCount, 0U) << "黑洞的 UDP 不该留下链路";
        ASSERT_EQ(outcome.http1IdleBetween, 0U);
        ASSERT_EQ(outcome.http2IdleBetween, 0U);

        EXPECT_GE(outcome.elapsed[0], 1500ms) << "第一条没等满探测时限：那条通路可能根本没被探（" << outcome.elapsed[0].count() << " ms）";
        EXPECT_LE(outcome.elapsed[1], 1500ms) << "第二条又付了一次探测时限：这个端点该记下「探败过」（" << outcome.elapsed[1].count() << " ms）";
    }

    /**
     * @brief 钉住流式上传绕开 h3：协议层的出站入口只收整份正文，这条请求该走 TCP
     * @details 对端只有 UDP 在听，因此「绕开了」的样子是连不上（状态码 0）而不是超时；判据写坏成
     *          「流式来源也交给 h3」时，那条通路会拿空正文提请求并拿到 200 与 h3 的正文——
     *          两种错法都会让这条用例红，而它同时钉住了「别把流式上传整块缓冲下来」。
     */
    TEST(HttpClientHttp3, StreamedUploadGoesOverTcp)
    {
        RunningPeer peer(PeerShape::Http3Only);
        ASSERT_TRUE(peer.awaitRunning());

        const Http3ClientRunOutcome outcome = runClientRequests({probeUrl(peer.port())}, true, {}, std::chrono::seconds{20}, true);
        ASSERT_EQ(outcome.statusCodes.size(), 1U);
        EXPECT_EQ(outcome.statusCodes[0], 0) << "带 bodySource 的请求走了 h3：那一条通路根本没有流式出口";
        EXPECT_EQ(outcome.http3LinkCount, 0U);
    }

    /**
     * @brief 钉住正文上限的接线：池里那一个数要落到 h3 的本端能力上，三条通路同解
     * @details 同一条大正文路由，配小上限的那台客户端该当场失败而不是收下它，配默认档的该收齐。只钉
     *          一侧会漏掉两种反方向：上限根本没接（大响应照收），与接错了把默认档也拒了。
     */
    TEST(HttpClientHttp3, BoundsResponseBodyByPoolConfig)
    {
        RunningPeer peer(PeerShape::Http3Only);
        ASSERT_TRUE(peer.awaitRunning());
        const std::string url = bigUrl(peer.port());

        HttpOutboundConnectionPool::Config tightConfig;
        tightConfig.maximumResponseBodyBytes = 1024U;
        const Http3ClientRunOutcome tight    = runClientRequests({url}, true, tightConfig);
        ASSERT_EQ(tight.statusCodes.size(), 1U);
        EXPECT_EQ(tight.statusCodes[0], 0) << "正文上限没接到 h3 这一侧：这么大的响应被照单全收";

        const Http3ClientRunOutcome defaultSized = runClientRequests({url}, true);
        ASSERT_EQ(defaultSized.statusCodes.size(), 1U);
        EXPECT_EQ(defaultSized.statusCodes[0], 200);
        EXPECT_EQ(defaultSized.bodies[0].size(), kBigBodyByteCount);
        EXPECT_EQ(defaultSized.bodies[0], std::string(kBigBodyByteCount, 'x')) << "大正文没按字节收齐";
    }
} // namespace AsynGyanis::Net
