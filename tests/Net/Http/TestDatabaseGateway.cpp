// 网关端到端：入站 HTTP 时限到点时，数据库里正在跑的语句要被驱动中断，连接按不可信丢弃，
// 池名额立刻回给后续请求。链路是 request.cancelToken() → PooledConnection::execute(stop_token)
// → DatabaseConnection::requestQueryCancel()，三段任何一段断开，本用例都会在时延或池读数上红。

#include "HttpTestSupport.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Routing/Middleware.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        constexpr auto kTimeout = std::chrono::seconds{10};

        /// 本机实测约 20 秒的递归统计：200 ms 的入站时限远早于它自然结束
        constexpr const char *kLongRunningCountSql = "WITH RECURSIVE tick (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM tick WHERE n < 200000000)"
                                                     " SELECT count(*) FROM tick";

        struct GatewayOutcome
        {
            std::string body;
            std::string reason;
        };

        /**
         * @brief 发一次 GET，把成功正文或失败原因落进调用方的记录
         * @details 写成具名协程而不是就地 lambda：闭包随表达式销毁，而帧里只存它的引用
         */
        Core::Task<void> sendOnceTask(Core::EventLoop &loop, std::string url, GatewayOutcome &outcome, const std::chrono::milliseconds requestTimeout)
        {
            try
            {
                const auto sent = co_await HttpClient::send(loop, url, HttpClientRequest{}, requestTimeout);
                if (sent.has_value())
                {
                    outcome.body = sent->body;
                } else
                {
                    outcome.reason = sent.error();
                }
            } catch (const std::exception &failure)
            {
                outcome.reason = failure.what();
            }
            loop.stop();
        }

        GatewayOutcome runSend(const std::string_view url, const std::chrono::milliseconds requestTimeout)
        {
            Core::EventLoop  loop;
            GatewayOutcome   outcome;
            Core::Task<void> work = sendOnceTask(loop, std::string(url), outcome, requestTimeout);
            if (!work.isReady())
            {
                loop.scheduler().schedule(work.handle());
            }
            loop.run();
            return outcome;
        }

        /**
         * @brief 建一个上限 1 的 SQLite 池：第二个请求要么拿到被腾出的名额，要么当场等满 100 ms
         * @return ConnectionPool 就绪的池
         */
        Database::ConnectionPool makeSingleConnectionSqlitePool()
        {
            Database::PoolConfig configuration;
            configuration.maximumPoolSize            = 1;
            configuration.acquireTimeoutMilliseconds = 100;
            return Database::ConnectionPool(
                    []()
                    {
                        std::unique_ptr<Database::DatabaseConnection> connection = Database::DatabaseFactory::createSqlite(Database::ConnectionConfig::sqliteDefault(":memory:"));
                        connection->connect();
                        return connection;
                    },
                    configuration);
        }
    } // namespace

    /**
     * @brief 钉住：入站 200 ms 时限打断正在跑的长查询，连接被丢弃，后续请求立刻拿到池名额
     */
    TEST(HttpDatabaseGateway, InboundDeadlineCancelsRunningQueryAndFreesThePoolConnection)
    {
        Database::ConnectionPool pool = makeSingleConnectionSqlitePool();
        // 必须在服务器循环之前声明：协程恢复投递到它，它要活到循环停手之后
        Core::AsyncExecutor executor(1);

        std::atomic<bool>        queryEntered{false};
        RunningHttpServerFixture server(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                        [&pool, &executor, &queryEntered](Router &router, Core::EventLoop &loop)
                                        {
                                            router.addMiddleware(timeoutMiddleware(loop, std::chrono::milliseconds{200}));
                                            router.get("/report",
                                                       [&pool, &executor, &queryEntered, &loop](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           Database::PooledConnection connection = pool.acquire();
                                                           if (!connection)
                                                           {
                                                               response.setStatus(503);
                                                               response.setBody("db-unavailable");
                                                               co_return;
                                                           }
                                                           auto                  lease = std::make_shared<Database::PooledConnection>(std::move(connection));
                                                           const std::stop_token token = request.cancelToken();
                                                           queryEntered.store(true, std::memory_order_release);
                                                           std::unique_ptr<Database::DatabaseResult> result = co_await executor.submit<std::unique_ptr<Database::DatabaseResult>>(
                                                                   loop, [lease, token] { return std::move(*lease).execute(kLongRunningCountSql, token); });
                                                           if (result == nullptr)
                                                           {
                                                               response.setStatus(503);
                                                               response.setBody("db-cancelled");
                                                               co_return;
                                                           }
                                                           response.setStatus(200);
                                                           response.setBody("done");
                                                       });
                                            router.get("/ping",
                                                       [&pool](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                                       {
                                                           Database::PooledConnection connection = pool.acquire();
                                                           if (!connection)
                                                           {
                                                               response.setStatus(503);
                                                               response.setBody("db-unavailable");
                                                               co_return;
                                                           }
                                                           std::unique_ptr<Database::DatabaseResult> result = std::move(connection).execute("SELECT 1");
                                                           if (result == nullptr)
                                                           {
                                                               response.setStatus(503);
                                                               response.setBody("db-unavailable");
                                                               co_return;
                                                           }
                                                           response.setStatus(200);
                                                           response.setBody("db-ok");
                                                       });
                                        });
        ASSERT_TRUE(server.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";
        const std::string baseUrl = "http://127.0.0.1:" + std::to_string(server.listeningPort());

        const auto           startedAt = std::chrono::steady_clock::now();
        const GatewayOutcome timedOut  = runSend(baseUrl + "/report", std::chrono::seconds{2});
        const auto           elapsed   = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        EXPECT_TRUE(queryEntered.load(std::memory_order_acquire)) << "长查询没有进入：本次判据的前提取不到";
        EXPECT_EQ(timedOut.body, "Gateway Timeout") << "入站时限没有把响应改写成 504，收到正文：" << timedOut.body << " 原因：" << timedOut.reason;
        EXPECT_LT(elapsed, std::chrono::seconds{1}) << "数据库语句没有中断：504 等到语句自然结束才发出（" << elapsed.count() << " ms）";
        EXPECT_EQ(pool.activeCount(), 0U) << "取消后连接仍记在借出账上：名额再也回不来";
        EXPECT_EQ(pool.idleCount(), 0U) << "中断过的连接被交回空闲栈：事务可能停在半条上";
        EXPECT_GE(pool.discardedCount(), 1U) << "取消没有把连接按不可信丢弃";

        // 最关键的一条：名额确实回池了，后续请求能立刻建新连接并执行
        const GatewayOutcome ping = runSend(baseUrl + "/ping", std::chrono::seconds{2});
        EXPECT_EQ(ping.body, "db-ok") << "取消后后续请求拿不到数据库：" << ping.reason;
    }
} // namespace AsynGyanis::Net
