find_package(GTest REQUIRED)

# 测试目录结构与 src/Database 逐级对应，全部链接进同一个 TestDatabase 可执行文件
add_executable(TestDatabase
        DatabaseTestSupport.h

        # 分配台账用例要替换全局 operator new 计数，实体定义在共用探针里（与 Base/Core/Net 同一份）
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/AllocationProbe.cpp

        Common/TestDatabaseTypes.cpp
        Common/TestDecimal.cpp
        Common/TestTemporalText.cpp
        Common/TestDatabaseFactory.cpp
        Common/TestDatabaseException.cpp
        Common/TestDatabaseConnection.cpp
        Common/TestDatabaseTlsPolicy.cpp

        Pool/TestConnectionPool.cpp
        Pool/TestConnectionPoolAsync.cpp
        Pool/TestConnectionPoolResilience.cpp
        Pool/TestPooledResultLease.cpp
        Pool/TestTransaction.cpp

        Dialect/TestSqliteDialect.cpp
        Dialect/TestMySqlDialect.cpp
        Dialect/TestPostgresDialect.cpp

        Queryable/TestQueryable.cpp
        Queryable/TestQueryableExecution.cpp
        Queryable/TestParameterizedExecution.cpp
        Queryable/TestRowMapper.cpp
        Queryable/TestQueryableOptimisticLock.cpp
        Queryable/TestQueryableRelations.cpp
        Queryable/TestQueryableRelationsAsync.cpp
        Queryable/TestSchemaMigrator.cpp
        Queryable/TestSchemaMigrations.cpp
        Queryable/TestQueryableAsync.cpp
        Queryable/TestOrmAllocations.cpp

        Sqlite/TestSqliteConnection.cpp
        Sqlite/TestSqliteResult.cpp

        MySql/TestMySqlConnection.cpp
        MySql/TestMySqlStatementResult.cpp
        MySql/TestMySqlValueConversion.cpp
        MySql/TestMySqlIntegration.cpp
        MySql/TestMySqlTls.cpp

        Postgres/TestPostgresConnection.cpp
        Postgres/TestPostgresValueConversion.cpp
        Postgres/TestPostgresIntegration.cpp

        Redis/TestRedisConnection.cpp
        Redis/TestRedisResult.cpp
        Redis/TestRedisSentinel.cpp
        Redis/TestRedisIntegration.cpp
        Redis/TestRedisTls.cpp
        Redis/TestRedisSubscription.cpp
)

target_link_libraries(TestDatabase PRIVATE
        Database
        GTest::gtest
        GTest::gtest_main
)

# src 用于 "Database/..." 形式的生产头包含，本目录用于 "DatabaseTestSupport.h" 同级包含，
# tests/Core 用于复用 CoreTestSupport.h 的协程驱动设施（等待、EventLoopThread）
target_include_directories(TestDatabase PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests/Core
        ${CMAKE_SOURCE_DIR}/tests/TestSupport
)

target_compile_features(TestDatabase PRIVATE cxx_std_23)

# 仓库自签证书夹具目录（TestMySqlTls 的「错误 CA」用例要指一个真实存在但与服务端不同源的证书）
target_compile_definitions(TestDatabase PRIVATE
        TEST_FIXTURES_DIR="${ASYN_TEST_FIXTURES_DIR}"
)

include(GoogleTest)
# RedisSubscriptionIntegration 一族会改服务端全局配置（notify-keyspace-events），而 ctest 逐用例
# 起进程：一条在等推送时，另一条的 TearDown 已还原全局值，前者收不到键空间事件（CI 实测间歇红，
# 本机 12 轮 × 4 并发红过 2 次）。资源锁串行化这一族；第二次发现调用另给 POST_FILE，防收尾互相覆盖。
gtest_discover_tests(TestDatabase
        TEST_FILTER "RedisSubscriptionIntegration.*"
        PROPERTIES RESOURCE_LOCK asyn-redis-server-config LABELS Database
        POST_FILE TestDatabase-subscription
        DISCOVERY_TIMEOUT ${ASYN_GTEST_DISCOVERY_TIMEOUT})
# MySqlIntegrationTest 一族共用库级对象（迁移登记表等物理表），并行时建表/删表会串进对方现场
# （实测 -j2 下单条用例间歇红，单跑与串行都过）。挂资源锁串行化；第二次发现调用另给 POST_FILE。
gtest_discover_tests(TestDatabase
        TEST_FILTER "MySqlIntegrationTest.*"
        PROPERTIES RESOURCE_LOCK asyn-mysql-integration LABELS Database
        POST_FILE TestDatabase-mysql
        DISCOVERY_TIMEOUT ${ASYN_GTEST_DISCOVERY_TIMEOUT})
# PostgresIntegrationTest 的迁移/种子用例共用库级迁移登记表，并行时一条 DROP 会抽走另一条的现场，
# 整族串行化；第二次发现调用另给 POST_FILE。
gtest_discover_tests(TestDatabase
        TEST_FILTER "PostgresIntegrationTest.*"
        PROPERTIES RESOURCE_LOCK asyn-postgres-integration LABELS Database
        POST_FILE TestDatabase-postgres
        DISCOVERY_TIMEOUT ${ASYN_GTEST_DISCOVERY_TIMEOUT})
gtest_discover_tests(TestDatabase
        TEST_FILTER "-RedisSubscriptionIntegration.*:MySqlIntegrationTest.*:PostgresIntegrationTest.*"
        PROPERTIES LABELS Database
        POST_FILE TestDatabase-rest
        DISCOVERY_TIMEOUT ${ASYN_GTEST_DISCOVERY_TIMEOUT})

# ASan 运行时拷到本目标输出目录（见 cmake/AsynGyanisSanitizers.cmake）
asyn_copy_sanitizer_runtime(TestDatabase)
