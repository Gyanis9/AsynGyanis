// 出站 keep-alive 连接池的端到端用例
#include "Base/Exception/InvalidArgumentException.h"
#include "HttpTestSupport.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpRequestBody.h"
#include "Platform/Platform.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
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

        constexpr auto kPooledWaitTimeout = std::chrono::seconds{10};

        /// 同一个 HttpClient 走完一串请求之后的结论
        struct PooledRunOutcome
        {
            std::vector<int>         statusCodes;            ///< 每条请求的状态码；0 表示这条失败了
            std::vector<std::string> bodies;                 ///< 每条请求的正文，用来认出「响应其实没回来」
            std::size_t              idleConnectionCount{0}; ///< 跑完时池里空闲着的连接条数
        };

        /**
         * @brief 在一条客户端事件循环上用同一个 HttpClient 走完 urls 里的每一条
         * @details 全部请求都在这一个循环、这一个客户端上跑完——「复用」这件事只有在同一条连接
         *          的归属循环上才成立，换循环就得换池。
         * @param loop 客户端事件循环
         * @param client 被测客户端（持有池）
         * @param urls 依次发出的地址
         * @param outcome 就地收集结论
         */
        Core::Task<void> runPooledTask(Core::EventLoop &loop, HttpClient &client, const std::vector<std::string> &urls, PooledRunOutcome &outcome)
        {
            for (const std::string &url: urls)
            {
                const std::unique_ptr<HttpClientResponse> response = co_await client.get(url);
                outcome.statusCodes.push_back(response ? response->statusCode : 0);
                outcome.bodies.push_back(response ? response->body : std::string{});
            }
            outcome.idleConnectionCount = client.idleConnectionCount();
            loop.stop();
            co_return;
        }

        /**
         * @brief 起一条事件循环、建一个带池的客户端，把 urls 走完
         * @param urls 依次发出的地址（同一个值重复 N 次就是「同主机连发 N 条」）
         * @param poolConfig 池参数；默认即产线口径
         * @return PooledRunOutcome 状态码与收尾时的空闲条数
         */
        PooledRunOutcome runPooledRequests(const std::vector<std::string> &urls, const HttpOutboundConnectionPool::Config poolConfig = {})
        {
            Core::EventLoop  loop;
            HttpClient       client(loop, poolConfig);
            PooledRunOutcome outcome;
            auto             work = runPooledTask(loop, client, urls, outcome);
            if (!work.isReady())
            {
                loop.scheduler().schedule(work.handle());
            }
            loop.run();
            return outcome;
        }

        std::string helloUrl(const std::uint16_t port)
        {
            return "http://127.0.0.1:" + std::to_string(port) + "/hello";
        }

        /// 「进门计数、按住不放」的 POST 路由路径
        constexpr std::string_view kSlowPostRoutePath = "/slow-post";

        /**
         * @brief 只讲字节的假对端：每条连接上每收完一份请求头就回同一份预设字节，且回完不关
         * @details 用来造自家服务端造不出来的线上形状——比如「本条响应之后还多带一截尾巴」的对端。
         *          这里不解析 HTTP，只按 \r\n\r\n 认请求头收齐；连接保持开着，好让「客户端到底复用
         *          了没」这件事由接入计数说了算，而不是被「对端先关了」抢走判据。
         *          网络库初始化由工作线程自己申请（见 TestUdpServer 同条纪律）：本框架之外自己 bind
         *          的套接字没人替它做这件事，而 ctest 是一用例一进程，别处初始化过也不算。
         */
        class RawBytePeer
        {
        public:
            explicit RawBytePeer(std::string payload) : m_payload(std::move(payload))
            {
            }

            RawBytePeer(const RawBytePeer &) = delete;

            RawBytePeer &operator=(const RawBytePeer &) = delete;

            ~RawBytePeer()
            {
                stop();
            }

            /**
             * @brief 起工作线程并等它把端口报回来
             * @param waitTimeout 等待端口的上限
             * @return true 已经在 127.0.0.1 的某个随机端口上监听
             */
            bool start(const std::chrono::milliseconds waitTimeout)
            {
                m_worker            = std::thread([this] { run(); });
                const auto deadline = std::chrono::steady_clock::now() + waitTimeout;
                while (m_port.load(std::memory_order_acquire) == 0U && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
                return m_port.load(std::memory_order_acquire) != 0U;
            }

            /// 收摊：置停止标志并join（各处轮询都带超时，最迟一拍就退出）
            void stop()
            {
                m_isStopping.store(true, std::memory_order_release);
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
            }

            [[nodiscard]] std::uint16_t port() const noexcept
            {
                return m_port.load(std::memory_order_acquire);
            }

            /// 接过几条连接：用例据此判「第二条请求走的是新连接还是复用来的那条」
            [[nodiscard]] std::size_t acceptedConnectionCount() const noexcept
            {
                return m_acceptedCount.load(std::memory_order_acquire);
            }

        private:
            void run()
            {
                const Platform::Socket::Initialization network;

                int listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(listener))
                {
                    return;
                }

                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = 0;
                if (::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listener, 8) != 0)
                {
                    Platform::FileDescriptor::close(listener);
                    return;
                }

                sockaddr_in bound{};
                // 长度参数的类型两家不同（Winsock 是 int*，POSIX 是 socklen_t*），一律按仓里的既有写法用 socklen_t
                socklen_t boundLength = static_cast<socklen_t>(sizeof(bound));
                if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &boundLength) != 0)
                {
                    Platform::FileDescriptor::close(listener);
                    return;
                }
                m_port.store(ntohs(bound.sin_port), std::memory_order_release);

                while (!m_isStopping.load(std::memory_order_acquire))
                {
                    fd_set readSet{};
                    FD_ZERO(&readSet);
                    FD_SET(listener, &readSet);
                    timeval pollInterval{0, 50 * 1000};
                    if (::select(listener + 1, &readSet, nullptr, nullptr, &pollInterval) <= 0)
                    {
                        continue; // 只是轮询停止标志
                    }

                    sockaddr_in peer{};
                    socklen_t   peerLength = static_cast<socklen_t>(sizeof(peer));
                    const int   client     = static_cast<int>(::accept(listener, reinterpret_cast<sockaddr *>(&peer), &peerLength));
                    if (!Platform::FileDescriptor::isValid(client))
                    {
                        continue;
                    }
                    m_acceptedCount.fetch_add(1U, std::memory_order_acq_rel);
                    serveEachRequest(client);
                    Platform::FileDescriptor::close(client);
                }
                Platform::FileDescriptor::close(listener);
            }

            /// 在这条连接上一次次回同一份预设字节，直到对端收口或本端收摊
            void serveEachRequest(const int client)
            {
                std::string            buffered;
                std::array<char, 1024> chunk{};
                while (!m_isStopping.load(std::memory_order_acquire))
                {
                    fd_set readSet{};
                    FD_ZERO(&readSet);
                    FD_SET(client, &readSet);
                    timeval   pollInterval{0, 50 * 1000};
                    const int readyCount = ::select(client + 1, &readSet, nullptr, nullptr, &pollInterval);
                    if (readyCount < 0)
                    {
                        return;
                    }
                    if (readyCount == 0)
                    {
                        continue;
                    }
                    const ssize_t received = ::recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
                    if (received <= 0)
                    {
                        return; // 对端收口或出错
                    }
                    buffered.append(chunk.data(), static_cast<std::size_t>(received));
                    if (buffered.find("\r\n\r\n") == std::string::npos)
                    {
                        continue; // 请求头还没收齐
                    }
                    buffered.clear();
                    static_cast<void>(::send(client, m_payload.data(), static_cast<int>(m_payload.size()), 0));
                }
            }

            std::string                m_payload;           ///< 每份请求要回的预设字节
            std::thread                m_worker;            ///< 接受与应答线程
            std::atomic<bool>          m_isStopping{false}; ///< 收摊标志
            std::atomic<std::uint16_t> m_port{0};           ///< 实际监听端口，0 表示还没起来
            std::atomic<std::size_t>   m_acceptedCount{0};  ///< 接过的连接条数
        };

        /**
         * @brief 注册一条「进门就计数、按住一段时间再回正文」的 POST 路由
         * @details 计数就是这条用例的判据：它数的是「这个请求被交付了几次」，比看响应更能认出
         *          「客户端悄悄重发了一遍」。按住的时间要长过客户端那次请求的时限，才会出现
         *          「请求已整个发出、还没答话」这一种本端分不清的情形。
         * @param router 目标路由器
         * @param loop 承载定时器的事件循环（服务端自己的）
         * @param entryCount 进门次数，由用例持有
         * @param holdTime 按住不放的时间
         */
        void registerCountedHoldingRoute(Router &router, Core::EventLoop &loop, std::atomic<std::size_t> &entryCount, const std::chrono::milliseconds holdTime)
        {
            router.post(std::string{kSlowPostRoutePath},
                        [&loop, &entryCount, holdTime](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                        {
                            entryCount.fetch_add(1U, std::memory_order_acq_rel);
                            Core::Timer holdTimer(loop);
                            co_await holdTimer.waitFor(holdTime);
                            response.setBody("served-slow-post");
                            co_return;
                        });
        }

        /// 一条池化 HTTP/1.1 连接被对端收掉之后再发一条 POST 的结论
        struct SlowPostRunOutcome
        {
            int         warmupStatusCode{0}; ///< 暖场那条 GET 的状态码；0 表示失败
            int         statusCode{0};       ///< 被测那条 POST 的状态码；0 表示失败
            std::size_t idleBefore{0};       ///< 暖场之后池里空闲着的连接条数
        };

        /**
         * @brief 在同一条循环、同一个客户端上跑「GET 暖场 → 宽限期等对端收掉 → 一条 POST → 再留一段落地时间」
         * @details 暖场是前提：没有池里那条可复用的连接，POST 走的就是「新连接」那一支，而那一支本来
         *          就不重试，测不到重发闸门。宽限期里本端不探测那条连接——留着的正是一份「看起来活着、
         *          其实已被对端收了」的存货。最后那段等待是给「悄悄重发的那一遍」留落地时间：它要重做
         *          DNS 与 TCP 连接，比正常路径慢。
         * @param loop 客户端事件循环
         * @param client 被测客户端（持有池）
         * @param warmUrl 暖场地址
         * @param slowUrl 那条 POST 的目标地址
         * @param idleGrace 留给服务端收口空闲连接的宽限期
         * @param outcome 就地收集结论
         */
        Core::Task<void> runWarmThenPostAfterIdleGrace(Core::EventLoop &loop, HttpClient &client, const std::string &warmUrl, const std::string &slowUrl,
                                                       const std::chrono::milliseconds idleGrace, SlowPostRunOutcome &outcome)
        {
            const std::unique_ptr<HttpClientResponse> warmup = co_await client.get(warmUrl);
            outcome.warmupStatusCode                         = warmup ? warmup->statusCode : 0;
            outcome.idleBefore                               = client.idleConnectionCount();
            Core::Timer graceTimer(loop);
            co_await graceTimer.waitFor(idleGrace);
            const std::unique_ptr<HttpClientResponse> posted = co_await client.post(slowUrl, "text/plain", "payload-once", std::chrono::milliseconds{2000});
            outcome.statusCode                               = posted ? posted->statusCode : 0;
            Core::Timer settleTimer(loop);
            co_await settleTimer.waitFor(std::chrono::milliseconds{800});
            loop.stop();
            co_return;
        }
    } // namespace

    /**
     * @brief 钉住池的本义：同一台主机的连续请求只占一条连接
     * @details 三条请求都拿到 200、客户端侧只剩一条空闲连接、服务端侧在册连接数也是 1。
     *          少任何一侧都不算证明：只看响应会漏掉「每条请求各开一条连接、旧的那条悄悄关掉」，
     *          只看客户端计数会漏掉「请求其实根本没发出去」。
     */
    TEST(HttpOutboundConnectionPool, ReusesOneConnectionAcrossSequentialRequests)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kPooledWaitTimeout)) << "服务端未在时限内进入接受循环";

        // 这一条不走 runPooledRequests 那个助手：客户端（连同它的池）必须在读服务端统计之前还活着
        // ——客户端析构会把空闲连接关掉，那时候服务端在册连接数已经归零，量到的就不是「复用了几条」
        Core::EventLoop   loop;
        HttpClient        client(loop);
        PooledRunOutcome  outcome;
        const std::string url = helloUrl(fixture.listeningPort());
        // urls 必须是个**具名**对象：驱动协程按引用拿着它，跨过 co_await 之后还要读它，
        // 直接传 {url, url, url} 的话这个临时 vector 在初始化语句结束时就没了
        const std::vector<std::string> urls{url, url, url};
        auto                           work = runPooledTask(loop, client, urls, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        ASSERT_EQ(outcome.statusCodes.size(), 3U);
        for (const int statusCode: outcome.statusCodes)
        {
            EXPECT_EQ(statusCode, 200) << "复用连接上的请求必须每条都成功";
        }
        for (const std::string &body: outcome.bodies)
        {
            EXPECT_NE(body.find("served-hello"), std::string::npos) << "正文：「" << body << "」";
        }
        EXPECT_EQ(outcome.idleConnectionCount, 1U) << "三条请求该只用一条连接";
        EXPECT_EQ(client.idleConnectionCount(), 1U);
        // 响应发出与计数落账之间隔着一次协程恢复，服务端侧的两个数都按条件轮询而不是立刻断言
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    const HttpServerStats stats = fixture.server().stats();
                    return stats.totalRequestCount >= 3U && stats.activeConnectionCount == 1U;
                },
                kPooledWaitTimeout))
                << "服务端侧的计数未在时限内落到「三条请求、一条连接」";
        EXPECT_EQ(fixture.server().stats().totalRequestCount, 3U) << "累计请求条数不符";
        EXPECT_EQ(fixture.server().stats().activeConnectionCount, 1U) << "在册连接数不对：说明每次都重开了连接";
    }

    /**
     * @brief 钉住：目标只有 IPv6 时出站请求走得通，池照样只复用一条
     * @details 修之前这条根本走不通：解析器给出 ::1，而 AsyncSocket::create 的默认档是 AF_INET，
     *          拿 IPv4 的套接字去 connect 一个 sockaddr_in6 只会以「协议族不符」收场。这里让服务器
     *          只监听 ::1，IPv6 就是唯一那条路。URL 里的方括号（RFC 3986 §3.2.2）要在拆分时去掉——
     *          带着括号去解析会得到一个并不存在的名字。第二条请求顺带钉住 keep-alive 与分组键在
     *          IPv6 上同样成立。这台机器没有可用的 IPv6 回环时按环境跳过，不报一条环境相关的红。
     */
    TEST(HttpOutboundConnectionPool, ReusesOneConnectionOverIpv6Loopback)
    {
        const auto fixture = tryStartHttpServerOn(Core::InetAddress{0, "::1"});
        if (fixture == nullptr)
        {
            GTEST_SKIP() << "::1 绑不上：这台机器没有可用的 IPv6 回环";
        }
        ASSERT_TRUE(fixture->awaitRunning(kPooledWaitTimeout)) << "服务端未在时限内进入接受循环";

        const std::string              url = "http://[::1]:" + std::to_string(fixture->listeningPort()) + "/hello";
        const std::vector<std::string> urls{url, url};
        const PooledRunOutcome         outcome = runPooledRequests(urls);

        ASSERT_EQ(outcome.statusCodes.size(), 2U);
        EXPECT_EQ(outcome.statusCodes[0], 200) << "连不上只监听 ::1 的服务器：IPv6 候选被拿去用 IPv4 的套接字连了";
        EXPECT_EQ(outcome.statusCodes[1], 200);
        for (const std::string &body: outcome.bodies)
        {
            EXPECT_NE(body.find("served-hello"), std::string::npos) << "正文：「" << body << "」";
        }
        EXPECT_EQ(outcome.idleConnectionCount, 1U) << "两条 IPv6 请求该共用一条连接，而不是每条各开一条";
    }

    /**
     * @brief 钉住：响应正文上限按池的配置生效，填 0 就是不限
     * @details 这道闸存在的理由是「正文多长由对端说了算」——不设上限就是让远端决定本进程分配多少
     *          内存。但它同时也拦得住正当的大响应，所以开关得在使用方手里，而且两头的行为都要钉：
     *          越界必须**整个拒下来**，不能悄悄截一段交回去（半截正文的形状是「200、长度也对、内容
     *          少了」，比一个错误难查一个量级）；填 0 则一条都不拦。
     */
    TEST(HttpOutboundConnectionPool, BoundsResponseBodyByConfiguredLimit)
    {
        constexpr std::size_t    kBodyByteCount = 4096U;
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/big",
                                                        [](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                                                        {
                                                            response.setBody(std::string(kBodyByteCount, 'x'));
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kPooledWaitTimeout)) << "服务端未在时限内进入接受循环";

        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/big";

        HttpOutboundConnectionPool::Config tightConfig;
        tightConfig.maximumResponseBodyBytes = 1024U;
        const PooledRunOutcome tight         = runPooledRequests({url}, tightConfig);
        ASSERT_EQ(tight.statusCodes.size(), 1U);
        EXPECT_EQ(tight.statusCodes[0], 0) << "越过本端上限的正文被收了：要么整个拒下来，要么别设闸";

        HttpOutboundConnectionPool::Config openConfig;
        openConfig.maximumResponseBodyBytes = 0U; ///< 0 表示不限
        const PooledRunOutcome open         = runPooledRequests({url}, openConfig);
        ASSERT_EQ(open.statusCodes.size(), 1U);
        EXPECT_EQ(open.statusCodes[0], 200) << "填 0 就该把上限放开：取大文件是正当用法";
        ASSERT_EQ(open.bodies.size(), 1U);
        EXPECT_EQ(open.bodies[0].size(), kBodyByteCount) << "正文长度不对：" << open.bodies[0].size();
    }

    /**
     * @brief 钉住对端声明 close 时不还回池里
     * @details 响应头里的 Connection: close 等于对端宣布这条连接到此为止（RFC 9112 §9.6）。留着它，
     *          下一条请求会写进一条正在收尾的连接；这里的判据是池里一条都不留，而第二条请求仍要成功
     *          （换新连接）。那个 token 的大小写不算数（`Connection: Close` 同解）——这一半判不在这里：
     *          本仓自家服务端会照自己的口径把这条连接收掉，端到端上两种写法长得一样，突变实测也证不出
     *          这一格会红，故按 `containsIgnoringCase` 的直测判（见 TestHttpHeaderRules）
     */
    TEST(HttpOutboundConnectionPool, DoesNotPoolConnectionThatPeerDeclaresClosed)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [](Router &router, Core::EventLoop &)
                                         {
                                             router.get("/closing",
                                                        [](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                                                        {
                                                            static_cast<void>(response.setHeader("connection", "close"));
                                                            response.setBody("served-closing");
                                                            co_return;
                                                        });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kPooledWaitTimeout)) << "服务端未在时限内进入接受循环";

        const std::string      url     = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/closing";
        const PooledRunOutcome outcome = runPooledRequests({url, url});
        ASSERT_EQ(outcome.statusCodes.size(), 2U);
        EXPECT_EQ(outcome.statusCodes[0], 200);
        EXPECT_EQ(outcome.statusCodes[1], 200) << "对端关掉上一条之后，第二条该换一条新连接重做而不是失败";
        EXPECT_EQ(outcome.idleConnectionCount, 0U) << "对端声明 close 的连接不该留在池里";
    }

    /**
     * @brief 钉住：本条响应之后还多带一截字节的对端，那条连接不许还回池里
     * @details 解析器只吃到响应结束那一格，同一批里剩下的字节属于「这条响应之外的东西」：对端多发的
     *          半条响应、第二条响应或一截尾巴。默默丢掉它们而把连接还回池里，下一条请求就从一个错位的
     *          读点开始解——上一位的尾巴会被当成本次的响应头读，这是响应走私那一族在客户端侧的形状。
     *          判据取两条且方向不同：第二条请求要落在一条**新**连接上（对端接过两次），以及跑完之后
     *          池里一条都不留。只数池不够——「暖场那条被对端关了」也会留下 0 条，却与本条要防的
     *          情形无关，所以接入计数才是能分辨这两者的那一位。
     */
    TEST(HttpOutboundConnectionPool, DoesNotPoolConnectionWithBytesLeftAfterResponse)
    {
        // 一条完整的 200（Content-Length: 2、正文 "hi"）之后紧跟一截不属于任何响应的尾巴，且回完不关连接：
        // 「对端还活着」这个前提要保住，才能让复用与否完全由被测那侧的决定说了算
        constexpr std::string_view kOverSentResponse = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"
                                                       "THIS-TRAILER-BELONGS-TO-NO-RESPONSE\r\n";

        RawBytePeer peer(std::string{kOverSentResponse});
        ASSERT_TRUE(peer.start(kPooledWaitTimeout)) << "假对端没能在时限内起来并报出端口";

        const std::string      url     = "http://127.0.0.1:" + std::to_string(peer.port()) + "/";
        const PooledRunOutcome outcome = runPooledRequests({url, url});

        ASSERT_EQ(outcome.statusCodes.size(), 2U);
        EXPECT_EQ(outcome.statusCodes[0], 200);
        EXPECT_EQ(outcome.statusCodes[1], 200) << "认出错位之后该换一条新连接重做，而不是把这一条也搭进去";
        ASSERT_EQ(outcome.bodies.size(), 2U);
        EXPECT_EQ(outcome.bodies[0], "hi");
        EXPECT_EQ(outcome.bodies[1], "hi");
        EXPECT_EQ(peer.acceptedConnectionCount(), 2U) << "响应之后还剩字节的连接被复用了：下一条请求会读到上一位的尾巴";
        EXPECT_EQ(outcome.idleConnectionCount, 0U) << "这种已经错位的连接一条都不该留在池里";
    }

    /**
     * @brief 钉住 keep-alive 的经典竞态：空闲期间被对端关掉的连接，下一次请求要自动重来一次
     * @details 服务端按 idleTimeout 收掉空闲连接，而这与客户端的取用之间没有任何协调——客户端只能
     *          「用了才知道」。判据是第二条请求仍然拿到 200（重开一条），而不是把这条竞态透给调用方。
     *          不做这层重试的连接池，在真实网络上就是偶发的空响应。
     */
    TEST(HttpOutboundConnectionPool, RecoversWhenPooledConnectionWasClosedByPeer)
    {
        HttpServerLimits limits;
        limits.idleTimeout = std::chrono::milliseconds{150};
        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{25});
        ASSERT_TRUE(fixture.awaitRunning(kPooledWaitTimeout)) << "服务端未在时限内进入接受循环";

        const std::string      url      = helloUrl(fixture.listeningPort());
        const PooledRunOutcome firstRun = runPooledRequests({url});
        ASSERT_EQ(firstRun.statusCodes.size(), 1U);
        EXPECT_EQ(firstRun.statusCodes[0], 200);
        EXPECT_EQ(firstRun.idleConnectionCount, 1U) << "第一条请求之后该留下一条可复用的连接";

        // 等过服务端的 keep-alive 空闲时限：池里那条连接已被对端收掉，而它自己还不知道
        std::this_thread::sleep_for(std::chrono::milliseconds{600});

        const PooledRunOutcome secondRun = runPooledRequests({url});
        ASSERT_EQ(secondRun.statusCodes.size(), 1U);
        EXPECT_EQ(secondRun.statusCodes[0], 200) << "复用来的连接被对端关掉时，该换一条新的重来一次而不是回空";
    }

    /**
     * @brief 钉住分组键：换了端口就是另一条通路，不能把连接错认成可复用
     * @details 键若只比主机，两个端口上的连接就会被混用——请求打到没跟它建立过连接的那台服务上
     *          （更糟的是把上一条连接的残留字节当成本条的响应头）。这里对两个端口各发一条，
     *          池里该留下两条。
     */
    TEST(HttpOutboundConnectionPool, KeepsIdleConnectionsGroupedByEndpoint)
    {
        RunningHttpServerFixture firstFixture(HttpServerLimits{}, std::chrono::milliseconds{100});
        RunningHttpServerFixture secondFixture(HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(firstFixture.awaitRunning(kPooledWaitTimeout));
        ASSERT_TRUE(secondFixture.awaitRunning(kPooledWaitTimeout));

        const PooledRunOutcome outcome = runPooledRequests({helloUrl(firstFixture.listeningPort()), helloUrl(secondFixture.listeningPort())});
        ASSERT_EQ(outcome.statusCodes.size(), 2U);
        EXPECT_EQ(outcome.statusCodes[0], 200);
        EXPECT_EQ(outcome.statusCodes[1], 200);
        EXPECT_EQ(outcome.idleConnectionCount, 2U) << "两个端口该各留一条空闲连接，而不是互相顶掉";
    }

    /**
     * @brief 钉住：closeIdleConnections() 真的把空闲连接收掉，客户端与服务端两侧一起归零
     * @details 这个方法的存在理由是「客户端还要留着，但手上的连接先放掉」——一轮突发出站结束后想把
     *          描述符还给系统，而不必等整个客户端析构。光靠取用时顺手清过期做不到这件事：一条还在
     *          空闲时限内的连接没人来取就一直占着两边。判据两头一起看——只量客户端那侧的话，
     *          closeAll() 里什么都不做也可能通过。
     */
    TEST(HttpOutboundConnectionPool, CloseIdleConnectionsReleasesSocketsOnBothSides)
    {
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kPooledWaitTimeout)) << "服务端未在时限内进入接受循环";

        // 客户端必须活到读完服务端统计之后（同复用那条用例的理由）：它一析构，在册连接数就自己归零了
        Core::EventLoop                loop;
        HttpClient                     client(loop);
        PooledRunOutcome               outcome;
        const std::vector<std::string> urls{helloUrl(fixture.listeningPort())};
        auto                           work = runPooledTask(loop, client, urls, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        ASSERT_EQ(outcome.idleConnectionCount, 1U) << "前置条件没成立：这条请求没在池里留下空闲连接";

        client.closeIdleConnections();

        EXPECT_EQ(client.idleConnectionCount(), 0U) << "调用之后池里还有存货";
        ASSERT_TRUE(waitForCondition([&fixture] { return fixture.server().stats().activeConnectionCount == 0U; }, kPooledWaitTimeout))
                << "服务端仍把那条连接记在册：本端只是丢了指针，没真的收口";
    }

    /**
     * @brief 钉住 HTTP/1.1 这一侧的重发闸门：请求已整个写上通路时，非幂等方法不重来一次
     * @details 场景取「对端在空闲期把连接收了」而不是「处理器太慢」：慢处理器那一条走的是时限放弃，
     *          整体预算已经用尽，重开连接那一步在算预算时就被挡住——那种场景证伪不了这道闸门
     *          （本条最初就是这么写的，撤掉闸门它照样绿）。对端收线则失败得很快、预算还剩一大截，
     *          撤掉闸门就会真的换一条新连接把同一条 POST 再交一次。
     * @details 判据是服务端的进入次数为 **0**：这条请求本端写过，但对端没接手过，而本端分不清这两种
     *          情形，只能按「可能已经执行过」处置。幂等方法（GET/HEAD/OPTIONS/PUT/DELETE/TRACE）
     *          允许重来一次，那就是上面 `RecoversWhenPooledConnectionWasClosedByPeer` 的恢复路径；
     *          RFC 9112 §9.3.2 给的自动重试许可本就只覆盖幂等方法。
     */
    TEST(HttpOutboundConnectionPool, DoesNotReplayASentNonIdempotentRequestWhenThePeerTookTheConnection)
    {
        std::atomic<std::size_t> postEntryCount{0U};
        HttpServerLimits         limits;
        limits.idleTimeout = std::chrono::milliseconds{150};
        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{25}, SlowRouteOptions{}, [&postEntryCount](Router &router, Core::EventLoop &serverLoop)
                                         { registerCountedHoldingRoute(router, serverLoop, postEntryCount, std::chrono::milliseconds{400}); });
        ASSERT_TRUE(fixture.awaitRunning(kPooledWaitTimeout)) << "服务端未在时限内进入接受循环";

        const std::string warmUrl = helloUrl(fixture.listeningPort());
        // 两个地址必须是具名对象：驱动协程按引用拿着它们，跨过 co_await 之后还要读
        const std::string  slowUrl = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + std::string{kSlowPostRoutePath};
        Core::EventLoop    loop;
        HttpClient         client(loop);
        SlowPostRunOutcome outcome;
        auto               work = runWarmThenPostAfterIdleGrace(loop, client, warmUrl, slowUrl, std::chrono::milliseconds{600}, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_EQ(outcome.warmupStatusCode, 200) << "暖场那条没成功：池里没有可复用的连接，被测那条走的就不是复用这一支";
        EXPECT_EQ(outcome.idleBefore, 1U) << "暖场之后池里不是一条连接：前提没成立";
        EXPECT_EQ(outcome.statusCode, 0) << "对端收了这条连接，本端分不清请求有没有被接手，不该给出一个成功";
        EXPECT_EQ(postEntryCount.load(std::memory_order_acquire), 0U) << "服务端收到了那条 POST：已整个写出的非幂等请求被换一条连接重发了一遍";
    }

    /**
     * @brief 钉住：流式上传是一段一段写上通路的，服务端按段收齐且拼回的正文完整
     * @details 判据用握手而不是计时：客户端生产第 k 段之前，先等服务端把第 k-1 批交付完（处理器的计数
     *          已抬起，见 registerStreamingEchoRoute）。于是「整份攒成一坨再发」那种退化会让服务端
     *          始终只看到 1 批，批次数当场报红；而等待预算用完时照常交正文，用例是**失败**不是挂住。
     * @details 另两条各拦一处：拼回的正文等于三段之和拦「丢段、重段、把终止块当正文发出去」（零长块
     *          按 RFC 9112 §7.1 就是终止块）；客户端拿到 200 拦「chunked 定界写坏，对端把请求读成
     *          不合规范」。
     */
    TEST(HttpOutboundConnectionPool, UploadsStreamedBodyAsChunkedRequest)
    {
        std::atomic<std::size_t> serverBatchCount{0};
        std::mutex               receivedGuard;
        std::string              receivedText;
        std::string              receivedContentType;
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [&serverBatchCount, &receivedGuard, &receivedText, &receivedContentType](Router &router, Core::EventLoop &)
                                         { registerStreamingEchoRoute(router, &serverBatchCount, &receivedGuard, &receivedText, &receivedContentType); });
        ASSERT_TRUE(fixture.awaitRunning(kPooledWaitTimeout)) << "服务端未在时限内进入接受循环";

        const std::string                              url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + std::string{kStreamEchoRoutePath};
        Core::EventLoop                                loop;
        std::expected<HttpClientResponse, std::string> outcome{std::unexpect, "还没跑"};
        auto                                           drive = [&loop, &url, &outcome, &serverBatchCount]() -> Core::Task<>
        {
            HttpClientRequest request;
            request.method      = "POST";
            request.contentType = "text/plain";
            request.bodySource  = makeStreamEchoChunkSource(loop, serverBatchCount);
            outcome             = co_await HttpClient::send(loop, url, request);
            loop.stop();
        };
        // 闭包先落到具名对象上再调用：协程帧记的是闭包地址，临时量在语句结束就析构，
        // 恢复时读的是死对象（容器里的 ASan 报 stack-use-after-scope）
        auto work = drive();
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        ASSERT_TRUE(outcome.has_value()) << "流式上传没走通：" << outcome.error();
        EXPECT_EQ(outcome->statusCode, 200);
        EXPECT_EQ(outcome->body, "echoed=3") << "服务端按批交付的次数不对：" << outcome->body;
        EXPECT_EQ(serverBatchCount.load(std::memory_order_acquire), kStreamEchoChunkCount) << "正文不是一段一段到服务端的";
        const std::lock_guard<std::mutex> guard(receivedGuard);
        EXPECT_EQ(receivedText, kStreamEchoExpectedText) << "拼回的正文：「" << receivedText << "」";
        // 这一条是 h1 的基线：媒体类型与承载协议无关，h2/h3 那两支共用同一份判据（少它就会被丢掉）
        EXPECT_EQ(receivedContentType, "text/plain") << "chunked 上传的媒体类型没上来：服务端收到的 content-type 是「" << receivedContentType << "」";
    }

    /**
     * @brief 钉住：整份正文与流式来源同时给属于用法错误，当场拒绝
     * @details 两种写法的定界头互斥（Content-Length 与 Transfer-Encoding: chunked），同时给等于让对端
     *          挑一份信，而挑哪一份由中间盒决定。口径同 URL 畸形与占用 owned 头部：抛出，不折进失败值。
     * @details 异常在协程体里抛、由 run() 记一条日志后重抛。这里就地接住而不是让 run() 往外抛：往外抛
     *          的话循环里定时器与协程帧的收尾顺序就不再是平时那一条，用例测的东西会跟着变。
     */
    TEST(HttpOutboundConnectionPool, RejectsRequestWithBothBufferedAndStreamedBody)
    {
        HttpClientRequest request;
        request.method     = "POST";
        request.body       = "whole-body";
        request.bodySource = []() -> Core::Task<std::optional<std::string>> { co_return std::nullopt; };
        Core::EventLoop loop;
        bool            isRejected{false};
        auto            drive = [&loop, &request, &isRejected]() -> Core::Task<>
        {
            try
            {
                const auto outcome = co_await HttpClient::send(loop, "http://127.0.0.1:1/stream-echo", request);
                static_cast<void>(outcome);
            } catch (const Base::InvalidArgumentException &)
            {
                isRejected = true;
            }
            loop.stop();
        };
        auto work = drive();
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_TRUE(isRejected) << "两种正文写法同时给却没被拒：定界头会互相矛盾";
    }
} // namespace AsynGyanis::Net
