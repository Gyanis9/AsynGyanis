find_package(GTest REQUIRED)

# 测试目录结构与 src/Database 逐级对应，全部链接进同一个 TestDatabase 可执行文件
add_executable(TestDatabase
        DatabaseTestSupport.h

        # 分配台账用例要替换全局 operator new 计数，实体定义在共用探针里（与 Base/Core/Net 同一份）
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/AllocationProbe.cpp

        Common/TestDatabaseTypes.cpp
        Common/TestDatabaseFactory.cpp
        Common/TestDatabaseException.cpp
        Common/TestDatabaseConnection.cpp
        Common/TestDatabaseTlsPolicy.cpp

        Pool/TestConnectionPool.cpp
        Pool/TestConnectionPoolAsync.cpp
        Pool/TestConnectionPoolResilience.cpp
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
        TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/Core/fixtures"
)

include(GoogleTest)
# RedisSubscriptionIntegration 这一族要改**服务端全局**配置（notify-keyspace-events），
# 而 ctest 是逐用例起进程的：一条还在等推送，另一条的 TearDown 就把全局值还原成原样，
# 于是前者再也收不到键空间事件（CI 实测会间歇红；本机 12 轮 × 4 并发红过 2 次）。
# 资源锁把这一族串行化，其余用例照旧并行。第二次发现调用要另给 POST_FILE，否则两份
# 收尾脚本互相覆盖。
gtest_discover_tests(TestDatabase
        TEST_FILTER "RedisSubscriptionIntegration.*"
        PROPERTIES RESOURCE_LOCK asyn-redis-server-config
        POST_FILE TestDatabase-subscription
        DISCOVERY_TIMEOUT ${ASYN_GTEST_DISCOVERY_TIMEOUT})
# MySqlIntegrationTest 这一族共用几张库级对象（迁移登记表、kMigratedTableName 之类的物理表），
# 而 ctest 逐用例起进程：并行时彼此的建表/删表会串进对方的现场（实测 -j2 下单条用例间歇红，
# 单跑与串行都过）。表名已按「各用例各持一张」改造过一轮，仍有低频残留，因此这一族挂资源锁
# 串行化——第二次发现调用要另给 POST_FILE，否则两份收尾脚本互相覆盖（同上一条的理由）。
gtest_discover_tests(TestDatabase
        TEST_FILTER "MySqlIntegrationTest.*"
        PROPERTIES RESOURCE_LOCK asyn-mysql-integration
        POST_FILE TestDatabase-mysql
        DISCOVERY_TIMEOUT ${ASYN_GTEST_DISCOVERY_TIMEOUT})
gtest_discover_tests(TestDatabase
        TEST_FILTER "-RedisSubscriptionIntegration.*:MySqlIntegrationTest.*"
        POST_FILE TestDatabase-rest
        DISCOVERY_TIMEOUT ${ASYN_GTEST_DISCOVERY_TIMEOUT})

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestDatabase)
