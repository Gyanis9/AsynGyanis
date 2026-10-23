// 池化结果集租约：结果集存活期间连接不得回到池里被下一个借用者复用。
// 只有 SQLite 的游标档（超过 256 行物化上限）真正引用连接，因此样本必须越过那个上限；
// 用例同时钉住「结果集活着的整段时间里，第二个借用者借不到这条连接」与「结果集析构后名额回池」。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Sqlite/SqliteResult.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
        /// 比 SqliteResult::kMaximumMaterializedRowCount 多，逼结果集走游标而不是整份物化
        constexpr int kRowCountBeyondMaterialization = static_cast<int>(SqliteResult::kMaximumMaterializedRowCount) + 44;

        /// 本机实测约 20 秒的递归统计：取消生效时毫秒级结束，失效时用例等到 5 秒判红
        constexpr const char *kLongRunningCountSql = "WITH RECURSIVE tick (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM tick WHERE n < 200000000)"
                                                     " SELECT count(*) FROM tick";

        /// 逐行产出的长查询：用来验证流式结果集后续 next() 也能被停止令牌打断
        constexpr const char *kLongStreamingRowsSql = "WITH RECURSIVE tick (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM tick WHERE n < 200000000)"
                                                      " SELECT n FROM tick";

        /**
         * @brief 建一个上限为 1 的 SQLite 池，库只在内存里
         * @param acquisitionTimeoutMilliseconds 第二个借用者愿意等多久
         * @return ConnectionPool 已就绪的池
         */
        ConnectionPool makeSingleConnectionSqlitePool(const int acquisitionTimeoutMilliseconds)
        {
            PoolConfig configuration;
            configuration.maximumPoolSize            = 1;
            configuration.acquireTimeoutMilliseconds = acquisitionTimeoutMilliseconds;
            return ConnectionPool(
                    []()
                    {
                        std::unique_ptr<DatabaseConnection> connection = DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault(":memory:"));
                        connection->connect();
                        return connection;
                    },
                    configuration);
        }

        /**
         * @brief 建表并灌入超过物化上限的行数
         * @param connection 池借出的唯一连接
         */
        void seedRowsBeyondMaterializationLimit(DatabaseConnection &connection)
        {
            ASSERT_NE(connection.execute("CREATE TABLE items (value INTEGER NOT NULL)"), nullptr) << connection.lastError();
            const std::string seedSql = "WITH RECURSIVE counter(value) AS (VALUES(1) UNION ALL SELECT value + 1 FROM counter WHERE value < " +
                                        std::to_string(kRowCountBeyondMaterialization) + ") INSERT INTO items SELECT value FROM counter";
            ASSERT_NE(connection.execute(seedSql), nullptr) << connection.lastError();
        }

        /**
         * @brief 钉住：结果集借走连接后，池不能再把它交给别人，直到结果集析构
         */
        TEST(PooledResultLease, ResultKeepsTheConnectionCheckedOutUntilDestroyed)
        {
            // 第二个借用者只等 100 毫秒：它必须等到超时，而不是拿到一条正在被游标读的连接
            ConnectionPool pool = makeSingleConnectionSqlitePool(100);

            {
                PooledConnection connection = pool.acquire();
                ASSERT_TRUE(connection) << "池里唯一一条连接都借不出来，前提不成立";
                seedRowsBeyondMaterializationLimit(*connection);
            }
            ASSERT_EQ(pool.idleCount(), 1U);
            ASSERT_EQ(pool.activeCount(), 0U);

            {
                PooledConnection connection = pool.acquire();
                ASSERT_TRUE(connection);
                // 右值限定：交出包装即把连接租给结果集，归还时机由结果集的析构决定
                std::unique_ptr<DatabaseResult> result = std::move(connection).execute("SELECT value FROM items ORDER BY value");
                ASSERT_NE(result, nullptr) << "查询失败，租约无从谈起";
                EXPECT_EQ(pool.idleCount(), 0U) << "结果集还活着，连接已经回到空闲栈：下一个借用者拿到同一条连接后，游标随时会被复用或关闭";
                EXPECT_EQ(pool.activeCount(), 1U);

                // 第二个借用者：同一个池、上限 1、截止 100 ms。连接若已回池它当场就能拿到
                PooledConnection contender = pool.acquire();
                EXPECT_FALSE(contender) << "结果集仍在逐行取数，第二条借出却拿到了同一条连接";
                EXPECT_EQ(contender.failureReason(), PoolAcquireFailure::TimedOut) << "失败原因不是等满截止时刻，说明连接根本不是被租约挡住的";

                // 结果集自己的取数不因这次失败的竞争受影响：连接没有被换掉
                std::size_t  rowsRead  = 0;
                std::int64_t lastValue = 0;
                while (result->next())
                {
                    lastValue = std::get<std::int64_t>(result->getValue(0));
                    ++rowsRead;
                }
                EXPECT_EQ(rowsRead, static_cast<std::size_t>(kRowCountBeyondMaterialization));
                EXPECT_EQ(lastValue, static_cast<std::int64_t>(kRowCountBeyondMaterialization));
            }

            EXPECT_EQ(pool.activeCount(), 0U) << "结果集析构后连接仍记在借出账上：这条名额再也不会被释放";
            EXPECT_EQ(pool.idleCount(), 1U) << "结果集析构后连接没有回到池里";
            EXPECT_EQ(pool.createdCount(), 1U) << "整个用例应当始终只有那一条连接";
        }

        /**
         * @brief 钉住：执行失败时连接不能被租出去，包装仍由调用方持有并可继续使用
         */
        TEST(PooledResultLease, FailedExecutionLeavesTheConnectionInTheWrapper)
        {
            ConnectionPool pool = makeSingleConnectionSqlitePool(100);

            PooledConnection connection = pool.acquire();
            ASSERT_TRUE(connection);

            // 表不存在：驱动置 lastError 并交回空结果集，连接必须原样留在包装里
            std::unique_ptr<DatabaseResult> failed = std::move(connection).execute("SELECT * FROM table_that_does_not_exist");
            EXPECT_EQ(failed, nullptr) << "这条语句本来就不该成功，前提不成立";
            EXPECT_TRUE(connection) << "执行失败后连接被租走：调用方既没拿到结果，也丢掉了这条连接";

            // 成功这一次才把连接租给结果集；结果集析构后名额回池
            std::unique_ptr<DatabaseResult> stream = std::move(connection).executeStreaming("SELECT 41");
            ASSERT_NE(stream, nullptr);
            ASSERT_TRUE(stream->next());
            EXPECT_EQ(std::get<std::int64_t>(stream->getValue(0)), 41);
            EXPECT_FALSE(connection) << "成功路径没有消耗包装，租约挂在哪条连接上就说不清了";
            stream.reset();
            EXPECT_EQ(pool.idleCount(), 1U);

            // 带参的两个重载走同一条租约：先 execute，再 executeStreaming，各自归还
            PooledConnection parameterized = pool.acquire();
            ASSERT_TRUE(parameterized);
            const std::vector<DatabaseValue> addends{std::int64_t{41}};
            std::unique_ptr<DatabaseResult>  sum = std::move(parameterized).execute("SELECT ? + 1", addends);
            ASSERT_NE(sum, nullptr);
            ASSERT_TRUE(sum->next());
            EXPECT_EQ(std::get<std::int64_t>(sum->getValue(0)), 42);
            sum.reset();
            EXPECT_EQ(pool.idleCount(), 1U);

            PooledConnection streamingParameterized = pool.acquire();
            ASSERT_TRUE(streamingParameterized);
            const std::vector<DatabaseValue> expectedValue{std::int64_t{7}};
            std::unique_ptr<DatabaseResult>  streamed = std::move(streamingParameterized).executeStreaming("SELECT ?", expectedValue);
            ASSERT_NE(streamed, nullptr);
            ASSERT_TRUE(streamed->next());
            EXPECT_EQ(std::get<std::int64_t>(streamed->getValue(0)), 7);
            streamed.reset();
            EXPECT_EQ(pool.activeCount(), 0U);
            EXPECT_EQ(pool.idleCount(), 1U);
        }

        /**
         * @brief 钉住：令牌在进入时已经停止，不启动语句、不消耗包装、也不丢弃连接
         */
        TEST(PooledResultLease, AlreadyStoppedTokenDoesNotStartOrConsumeTheConnection)
        {
            ConnectionPool pool = makeSingleConnectionSqlitePool(100);

            PooledConnection connection = pool.acquire();
            ASSERT_TRUE(connection);

            std::stop_source cancelSource;
            cancelSource.request_stop();
            std::unique_ptr<DatabaseResult> result = std::move(connection).execute("SELECT 1", cancelSource.get_token());
            EXPECT_EQ(result, nullptr);
            EXPECT_TRUE(connection) << "令牌进入时已停止：不该启动语句，也不该把连接租出去或丢弃";

            // 没有语句被中断过，连接仍可直接使用
            std::unique_ptr<DatabaseResult> ok = std::move(connection).execute("SELECT 1");
            ASSERT_NE(ok, nullptr);
            ASSERT_TRUE(ok->next());
            EXPECT_EQ(std::get<std::int64_t>(ok->getValue(0)), 1);
            ok.reset();
            EXPECT_EQ(pool.idleCount(), 1U);
            EXPECT_EQ(pool.discardedCount(), 0U);
        }

        /**
         * @brief 钉住：执行期间停止令牌，驱动被打断、连接按不可信丢弃、名额立刻回池
         * @details 查询本身约 20 秒且 queryTimeout 设 0，能在毫秒级返回只可能是停止令牌接进了驱动。
         */
        TEST(PooledResultLease, CancellationInterruptsTheStatementAndDiscardsTheConnection)
        {
            ConnectionPool pool = makeSingleConnectionSqlitePool(100);

            PooledConnection connection = pool.acquire();
            ASSERT_TRUE(connection);
            connection->setQueryTimeout(0);

            std::stop_source                cancelSource;
            std::atomic<bool>               finished{false};
            std::unique_ptr<DatabaseResult> result;
            const auto                      startedAt = std::chrono::steady_clock::now();
            std::jthread                    worker(
                    [connection = std::move(connection), token = cancelSource.get_token(), &finished, &result]() mutable
                    {
                        result = std::move(connection).execute(kLongRunningCountSql, token);
                        finished.store(true, std::memory_order_release);
                    });

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cancelSource.request_stop();

            const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < cancelDeadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ASSERT_TRUE(finished.load(std::memory_order_acquire)) << "取消之后语句仍在跑：池化执行没有把停止令牌接到驱动";
            worker.join();
            const auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();

            EXPECT_EQ(result, nullptr);
            EXPECT_LT(elapsedMilliseconds, 5000) << "耗时 " << elapsedMilliseconds << " 毫秒，不像是被取消打断的";
            EXPECT_EQ(pool.activeCount(), 0U) << "取消后连接仍记在借出账上";
            EXPECT_EQ(pool.idleCount(), 0U) << "中断过的连接被交回空闲栈：事务可能停在半条上";
            EXPECT_GE(pool.discardedCount(), 1U) << "取消没有把连接按不可信丢弃";

            // 池仍可用：丢弃腾出的名额让下一次借出建一条新连接
            PooledConnection replacement = pool.acquire();
            ASSERT_TRUE(replacement);
            std::unique_ptr<DatabaseResult> ok = std::move(replacement).execute("SELECT 1");
            ASSERT_NE(ok, nullptr) << "取消之后池给不出可用连接";
            ASSERT_TRUE(ok->next());
            EXPECT_EQ(std::get<std::int64_t>(ok->getValue(0)), 1);
            ok.reset();
            EXPECT_EQ(pool.idleCount(), 1U);
            EXPECT_EQ(pool.createdCount(), 1U) << "丢弃后账上应只剩新借出的这一条：旧的那条不该还挂在计数里";
        }

        /**
         * @brief 钉住：流式结果集的 next() 也认停止令牌；取消后连接在结果集析构时被丢弃
         * @details 令牌如果在 executeStreaming 返回时就摘掉，逐行取数期间请求停止不会有任何效果，
         *          200M 行的迭代会一直跑到自然结束——用例在 5 秒处判红。
         */
        TEST(PooledResultLease, CancellationDuringStreamingIterationInterruptsAndDiscards)
        {
            ConnectionPool pool = makeSingleConnectionSqlitePool(100);

            PooledConnection connection = pool.acquire();
            ASSERT_TRUE(connection);
            connection->setQueryTimeout(0);

            std::stop_source                cancelSource;
            std::unique_ptr<DatabaseResult> stream = std::move(connection).executeStreaming(kLongStreamingRowsSql, cancelSource.get_token());
            ASSERT_NE(stream, nullptr) << "流式查询没建起来，前提不成立";

            std::atomic<bool> finished{false};
            std::atomic<bool> sawRow{false};
            std::jthread      worker(
                    [&stream, &finished, &sawRow]
                    {
                        while (stream->next())
                        {
                            sawRow.store(true, std::memory_order_release);
                        }
                        finished.store(true, std::memory_order_release);
                    });

            // 先确认迭代真的在逐行推进，再请求停止：否则「取消生效」可能只是查询还没开始
            const auto rowDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!sawRow.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < rowDeadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            ASSERT_TRUE(sawRow.load(std::memory_order_acquire)) << "流式迭代没有产出任何行，前提不成立";
            cancelSource.request_stop();

            const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < stopDeadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            ASSERT_TRUE(finished.load(std::memory_order_acquire)) << "取消之后 next() 仍在推进：流式迭代没有接停止令牌";
            worker.join();

            EXPECT_FALSE(stream->lastError().empty()) << "被打断的流式迭代没有留下原因";
            stream.reset();
            EXPECT_EQ(pool.activeCount(), 0U);
            EXPECT_EQ(pool.idleCount(), 0U) << "取消过的流式连接被交回空闲栈：游标/事务可能停在半条上";
            EXPECT_GE(pool.discardedCount(), 1U);

            PooledConnection replacement = pool.acquire();
            ASSERT_TRUE(replacement);
            std::unique_ptr<DatabaseResult> ok = std::move(replacement).execute("SELECT 1");
            ASSERT_NE(ok, nullptr) << "取消之后池给不出可用连接：" << pool.activeCount();
            ok.reset();
        }
    } // namespace
} // namespace AsynGyanis::Database
