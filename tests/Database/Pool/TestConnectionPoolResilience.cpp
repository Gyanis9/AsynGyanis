// 连接池的失联处置：借出前的有效性探针、建连的有界重试，以及「这次建连失败值不值得再试」的分类表。
// 全部走 TestConnectionPool.h 的打桩连接，不触碰任何真实数据库；取出统一用 tryAcquire()
// （单轮、不排队），这样「工厂被调用了几次」就是一个确定的数，而不是等超时等出来的运气。

#include "Database/Common/DatabaseConnection.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"

#include "TestConnectionPool.h"
#include "MetricsTestSupport.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace AsynGyanis::Database
{
    namespace
    {
        using AsynGyanis::TestSupport::hasRegistrySample;
        using AsynGyanis::TestSupport::registryValue;
        using TestPoolSupport::ConnectionCounter;
        using TestPoolSupport::MockConnection;

        /// 造一个只建一条连接的池配置：探针与重试两项各自要几条尝试，用例里写死
        PoolConfig resilienceConfig(const std::size_t maximumConnectAttempts)
        {
            PoolConfig config;
            config.maximumPoolSize = 1;
            // 时限给足但不排队（tryAcquire 本来就不等）：重试的总预算挂在这项上，填 0 就等于
            // 「一次都不重试」，那测的是另一条判据，见 DefaultsKeepThePreExistingBehaviour
            config.acquireTimeoutMilliseconds = 5000;
            config.maximumConnectAttempts     = maximumConnectAttempts;
            return config;
        }

        /// 把一条打桩连接推到「刚失败过」的状态，好按它的错误码问分类
        [[nodiscard]] std::unique_ptr<MockConnection> makeFailedConnection(ConnectionCounter &counter, const DatabaseType type, const std::int64_t nativeCode)
        {
            counter.connectFailuresRemaining.store(1);
            counter.connectFailureNativeCode.store(nativeCode);
            auto connection = std::make_unique<MockConnection>(counter, 1);
            connection->setDatabaseType(type);
            static_cast<void>(connection->connect());
            return connection;
        }
    } // namespace

    TEST(ConnectionPoolResilience, SendsNoProbeByDefault)
    {
        // 判据：这项开关关掉时一次探针都不发——打开它等于每次借用多一次往返，默认必须是关的
        ConnectionCounter counter;
        PoolConfig        config;
        config.maximumPoolSize = 1;
        ConnectionPool pool(TestPoolSupport::makeMockFactory(counter), config);

        PooledConnection first = pool.tryAcquire();
        ASSERT_TRUE(first);
        first.release();

        PooledConnection second = pool.tryAcquire();
        ASSERT_TRUE(second);
        EXPECT_EQ(counter.probeCount.load(), 0);
    }

    TEST(ConnectionPoolResilience, ProbesOnlyIdleConnectionsWhenEnabled)
    {
        // 判据两条：① 新建的那条不探（刚握完手，探一次是白付的往返）；② 从空闲栈取出的那条才探
        ConnectionCounter counter;
        PoolConfig        config;
        config.maximumPoolSize    = 1;
        config.probeBeforeAcquire = true;
        ConnectionPool pool(TestPoolSupport::makeMockFactory(counter), config);

        PooledConnection first = pool.tryAcquire();
        ASSERT_TRUE(first);
        EXPECT_EQ(counter.probeCount.load(), 0);
        first.release();

        PooledConnection second = pool.tryAcquire();
        ASSERT_TRUE(second);
        EXPECT_EQ(counter.probeCount.load(), 1);
    }

    TEST(ConnectionPoolResilience, DropsAConnectionThatFailsTheProbeAndHandsOutAFreshOne)
    {
        // 判据：状态读起来还「连着」、探针说不通——这条不能交出去。丢弃、补建、把新的交给同一次借用，
        // 借用者不该看到空手，也不该拿到那条死人
        ConnectionCounter counter;
        PoolConfig        config;
        config.maximumPoolSize    = 2;
        config.probeBeforeAcquire = true;
        ConnectionPool pool(TestPoolSupport::makeMockFactory(counter), config);

        PooledConnection first = pool.tryAcquire();
        ASSERT_TRUE(first);
        first.release();
        ASSERT_EQ(counter.totalCreated.load(), 1);

        counter.connectionsProbeOk.store(false);
        PooledConnection second = pool.tryAcquire();
        counter.connectionsProbeOk.store(true);

        ASSERT_TRUE(second) << "探针判死之后没有补建，借用者白等了一次";
        EXPECT_EQ(counter.totalCreated.load(), 2);
        EXPECT_EQ(pool.discardedCount(), 1u);
        EXPECT_EQ(counter.probeCount.load(), 1);
    }

    TEST(ConnectionPoolResilience, RetriesTransientConnectFailuresUpToTheConfiguredAttempts)
    {
        // 判据：前两次报「锁竞争/连不上」那一类，第三次成——借用最终拿到连接，工厂正好被叫三次
        ConnectionCounter counter;
        counter.connectFailuresRemaining.store(2);
        counter.connectFailureNativeCode.store(5); // SQLITE_BUSY：等一会再来就行

        ConnectionPool   pool(TestPoolSupport::makeMockFactory(counter), resilienceConfig(3));
        PooledConnection connection = pool.tryAcquire();

        EXPECT_TRUE(connection);
        EXPECT_EQ(counter.totalCreated.load(), 2 + 1); // 失败的两次 + 成功的一次
    }

    TEST(ConnectionPoolResilience, DoesNotRetryAFailureTheDriverSaysIsFatal)
    {
        // 判据：驱动报的是「打不开文件」这一类，重试也不会中——一次就空手，别把借用拖成长等
        ConnectionCounter counter;
        counter.connectFailuresRemaining.store(5);
        counter.connectFailureNativeCode.store(14); // SQLITE_CANTOPEN

        ConnectionPool   pool(TestPoolSupport::makeMockFactory(counter), resilienceConfig(3));
        PooledConnection connection = pool.tryAcquire();

        EXPECT_FALSE(connection);
        EXPECT_EQ(counter.totalCreated.load(), 1);
        EXPECT_EQ(counter.connectFailuresRemaining.load(), 4);
    }

    TEST(ConnectionPoolResilience, StopsRetryingAtTheAttemptCap)
    {
        // 判据：可重试的错误也不许无限试——次数上限就是这次借用愿意付的代价，到顶即空手
        ConnectionCounter counter;
        counter.connectFailuresRemaining.store(5);
        counter.connectFailureNativeCode.store(5);

        ConnectionPool   pool(TestPoolSupport::makeMockFactory(counter), resilienceConfig(2));
        PooledConnection connection = pool.tryAcquire();

        EXPECT_FALSE(connection);
        EXPECT_EQ(counter.totalCreated.load(), 2);
        EXPECT_EQ(counter.connectFailuresRemaining.load(), 3);
    }

    TEST(ConnectionPoolResilience, ClassifiesTransientConnectFailuresPerDriver)
    {
        // 判据是一张表而不是一个 bool：各驱动的错误码词汇表互不相通，混档就是拿 MySQL 的码去比 SQLite 的表
        ConnectionCounter counter;

        EXPECT_TRUE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::Sqlite, 5)));
        EXPECT_TRUE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::Sqlite, 6)));
        EXPECT_FALSE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::Sqlite, 14)));

        EXPECT_TRUE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::MySql, 2002)));
        EXPECT_TRUE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::MySql, 2003)));
        EXPECT_FALSE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::MySql, 1045)));
        // 2006/2013 是「语句发到一半连接断了」：写可能已经生效，重做整件事归调用方决定，
        // 池不在这里替它重放（这也是 isRetryable() 刻意不含它们的同一条理由）
        EXPECT_FALSE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::MySql, 2006)));
        EXPECT_FALSE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::MySql, 2013)));

        EXPECT_TRUE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::PostgreSql, postgresSqlStateNativeCode("08001"))));
        EXPECT_TRUE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::PostgreSql, postgresSqlStateNativeCode("57P01"))));
        EXPECT_FALSE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::PostgreSql, postgresSqlStateNativeCode("3D000"))));
        EXPECT_FALSE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::PostgreSql, postgresSqlStateNativeCode("28P01"))));

        EXPECT_TRUE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::Redis, ECONNREFUSED)));
        EXPECT_TRUE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::Redis, ETIMEDOUT)));
        EXPECT_FALSE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::Redis, ENOENT)));
        // 「驱动没给出码」的失败（只写了文本）一律不重试：不知道原因就不该再花一次往返
        EXPECT_FALSE(isTransientConnectFailure(*makeFailedConnection(counter, DatabaseType::Redis, -1)));
    }

    TEST(ConnectionPoolResilience, StopsRetryingOnceTheBorrowDeadlinePasses)
    {
        // 判据：次数上限之外还有一道「这次借用愿意等的总时长」。这里取的是**上界**而不是第几轮撞线——
        // 撞上的是真实时钟，具体停在哪一轮由这台机器怎么排这几觉决定，钉死某一轮就是把用例交给调度运气
        ConnectionCounter counter;
        counter.connectFailuresRemaining.store(50);
        counter.connectFailureNativeCode.store(5);

        PoolConfig config;
        config.maximumPoolSize            = 1;
        config.acquireTimeoutMilliseconds = 30;
        config.maximumConnectAttempts     = 50;

        ConnectionPool pool(TestPoolSupport::makeMockFactory(counter), config);
        EXPECT_FALSE(pool.tryAcquire());
        // 真跑满 50 次至少要睡 5+10+20+40+100×46 毫秒，那道 30 毫秒的闸门必然先落下；
        // 把闸门拆掉则这里正好读到 50
        EXPECT_LT(counter.totalCreated.load(), 50u) << "总时长闸门没落下，重试把一次借用变成了一次长等";
    }

    TEST(ConnectionPoolResilience, PublishesProbeAndRetryReadoutsToTheProcessRegistry)
    {
        // 判据：两条读数不是「登记上就算有了」，它们必须被借出路径真推着走。名字在构造时就挂在导出里、
        // 值读到 0 那一格由 ConnectionPoolMetrics 钉；这里钉的是增量，以及「第一次建连不算重试」这个口径
        ConnectionCounter counter;
        counter.connectFailuresRemaining.store(2);
        counter.connectFailureNativeCode.store(5);

        PoolConfig config;
        config.maximumPoolSize            = 1;
        config.acquireTimeoutMilliseconds = 5000;
        config.probeBeforeAcquire         = true;
        config.maximumConnectAttempts     = 3;
        ConnectionPool pool(TestPoolSupport::makeMockFactory(counter), config);

        ASSERT_TRUE(hasRegistrySample("asyn_db_pool_connection_probes_total"));
        ASSERT_TRUE(hasRegistrySample("asyn_db_pool_connect_retries_total"));
        EXPECT_EQ(registryValue("asyn_db_pool_connection_probes_total"), 0U);

        {
            const PooledConnection borrowed = pool.tryAcquire();
            ASSERT_TRUE(borrowed);
            // 三次尝试、两次失败：重试记 2 而不是 3
            EXPECT_EQ(registryValue("asyn_db_pool_connect_retries_total"), 2U) << "重试读数没记满，或把第一次建连也算成了重试";
        }

        EXPECT_EQ(registryValue("asyn_db_pool_connection_probes_total"), 0U) << "刚建好的那条被白探了一次";

        {
            const PooledConnection reused = pool.tryAcquire();
            ASSERT_TRUE(reused);
        }
        EXPECT_EQ(registryValue("asyn_db_pool_connection_probes_total"), 1U) << "从空闲栈取出的那条没记进探针读数";
    }

    TEST(ConnectionPoolResilience, DefaultsKeepThePreExistingBehaviour)
    {
        // 判据：两个新开关的默认值必须等于「这项不存在时的行为」——探针不发、建连失败一次就空手
        const PoolConfig defaults;
        EXPECT_FALSE(defaults.probeBeforeAcquire);
        EXPECT_EQ(defaults.maximumConnectAttempts, 1u);

        ConnectionCounter counter;
        counter.connectFailuresRemaining.store(3);
        counter.connectFailureNativeCode.store(5);
        ConnectionPool pool(TestPoolSupport::makeMockFactory(counter), PoolConfig{});
        EXPECT_FALSE(pool.tryAcquire());
        EXPECT_EQ(counter.totalCreated.load(), 1);
    }
} // namespace AsynGyanis::Database
