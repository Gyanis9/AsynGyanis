// 出站 keep-alive 连接池的端到端用例
#include "HttpTestSupport.h"
#include "Net/Http/Client/HttpClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
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
            std::vector<int> statusCodes;             ///< 每条请求的状态码；0 表示这条失败了
            std::vector<std::string> bodies;          ///< 每条请求的正文，用来认出「响应其实没回来」
            std::size_t idleConnectionCount{0};       ///< 跑完时池里空闲着的连接条数
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
        Core::Task<void> runPooledTask(Core::EventLoop &loop, HttpClient &client, const std::vector<std::string> &urls,
                                       PooledRunOutcome &outcome)
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
        PooledRunOutcome runPooledRequests(const std::vector<std::string> &urls,
                                           const HttpOutboundConnectionPool::Config poolConfig = {})
        {
            Core::EventLoop loop;
            HttpClient client(loop, poolConfig);
            PooledRunOutcome outcome;
            auto work = runPooledTask(loop, client, urls, outcome);
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
        Core::EventLoop loop;
        HttpClient client(loop);
        PooledRunOutcome outcome;
        const std::string url = helloUrl(fixture.listeningPort());
        // urls 必须是个**具名**对象：驱动协程按引用拿着它，跨过 co_await 之后还要读它，
        // 直接传 {url, url, url} 的话这个临时 vector 在初始化语句结束时就没了
        const std::vector<std::string> urls{url, url, url};
        auto work = runPooledTask(loop, client, urls, outcome);
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
                kPooledWaitTimeout)) << "服务端侧的计数未在时限内落到「三条请求、一条连接」";
        EXPECT_EQ(fixture.server().stats().totalRequestCount, 3U) << "累计请求条数不符";
        EXPECT_EQ(fixture.server().stats().activeConnectionCount, 1U) << "在册连接数不对：说明每次都重开了连接";
    }

    /**
     * @brief 钉住对端声明 close 时不还回池里
     * @details 响应头里的 Connection: close 等于对端宣布这条连接到此为止（RFC 9112 §9.6）。留着它，
     *          下一条请求会写进一条正在收尾的连接；这里的判据是池里一条都不留，而第二条请求仍要成功
     *          （换新连接）。
     */
    TEST(HttpOutboundConnectionPool, DoesNotPoolConnectionThatPeerDeclaresClosed)
    {
        RunningHttpServerFixture fixture(
                HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                [](Router &router, Core::EventLoop &)
                {
                    router.get("/closing", [](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                    {
                        static_cast<void>(response.setHeader("connection", "close"));
                        response.setBody("served-closing");
                        co_return;
                    });
                });
        ASSERT_TRUE(fixture.awaitRunning(kPooledWaitTimeout)) << "服务端未在时限内进入接受循环";

        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/closing";
        const PooledRunOutcome outcome = runPooledRequests({url, url});
        ASSERT_EQ(outcome.statusCodes.size(), 2U);
        EXPECT_EQ(outcome.statusCodes[0], 200);
        EXPECT_EQ(outcome.statusCodes[1], 200) << "对端关掉上一条之后，第二条该换一条新连接重做而不是失败";
        EXPECT_EQ(outcome.idleConnectionCount, 0U) << "对端声明 close 的连接不该留在池里";
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

        const std::string url = helloUrl(fixture.listeningPort());
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

        const PooledRunOutcome outcome = runPooledRequests({helloUrl(firstFixture.listeningPort()),
                                                            helloUrl(secondFixture.listeningPort())});
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
        Core::EventLoop loop;
        HttpClient client(loop);
        PooledRunOutcome outcome;
        const std::vector<std::string> urls{helloUrl(fixture.listeningPort())};
        auto work = runPooledTask(loop, client, urls, outcome);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        ASSERT_EQ(outcome.idleConnectionCount, 1U) << "前置条件没成立：这条请求没在池里留下空闲连接";

        client.closeIdleConnections();

        EXPECT_EQ(client.idleConnectionCount(), 0U) << "调用之后池里还有存货";
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().stats().activeConnectionCount == 0U;
                },
                kPooledWaitTimeout)) << "服务端仍把那条连接记在册：本端只是丢了指针，没真的收口";
    }
} // namespace AsynGyanis::Net
