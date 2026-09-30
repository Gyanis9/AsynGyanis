find_package(GTest REQUIRED)

# 测试目录结构与 src/Database 逐级对应，全部链接进同一个 TestDatabase 可执行文件
add_executable(TestDatabase
        DatabaseTestSupport.h

        # 分配台账用例要替换全局 operator new 计数，实体定义在共用探针里（与 Base/Core/Net 同一份）
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/AllocationProbe.cpp

        Common/TestDatabaseTypes.cpp
        Common/TestDatabaseFactory.cpp
        Common/TestDatabaseException.cpp

        Pool/TestConnectionPool.cpp
        Pool/TestConnectionPoolAsync.cpp
        Pool/TestTransaction.cpp

        Dialect/TestSqliteDialect.cpp
        Dialect/TestMySqlDialect.cpp

        Queryable/TestQueryable.cpp
        Queryable/TestQueryableExecution.cpp
        Queryable/TestParameterizedExecution.cpp
        Queryable/TestRowMapper.cpp
        Queryable/TestSchemaMigrator.cpp
        Queryable/TestQueryableAsync.cpp
        Queryable/TestOrmAllocations.cpp

        Sqlite/TestSqliteConnection.cpp
        Sqlite/TestSqliteResult.cpp

        MySql/TestMySqlConnection.cpp
        MySql/TestMySqlStatementResult.cpp
        MySql/TestMySqlValueConversion.cpp
        MySql/TestMySqlIntegration.cpp


        Redis/TestRedisConnection.cpp
        Redis/TestRedisResult.cpp
        Redis/TestRedisIntegration.cpp
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
gtest_discover_tests(TestDatabase
        TEST_FILTER "-RedisSubscriptionIntegration.*"
        POST_FILE TestDatabase-rest
        DISCOVERY_TIMEOUT ${ASYN_GTEST_DISCOVERY_TIMEOUT})

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestDatabase)
