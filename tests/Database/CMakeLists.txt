find_package(GTest REQUIRED)

# 测试目录结构与 src/Database 逐级对应，全部链接进同一个 TestDatabase 可执行文件
add_executable(TestDatabase
        DatabaseTestSupport.h

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

        Sqlite/TestSqliteConnection.cpp
        Sqlite/TestSqliteResult.cpp

        MySql/TestMySqlConnection.cpp
        MySql/TestMySqlStatementResult.cpp
        MySql/TestMySqlValueConversion.cpp
        MySql/TestMySqlIntegration.cpp


        Redis/TestRedisConnection.cpp
        Redis/TestRedisResult.cpp
        Redis/TestRedisIntegration.cpp
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
gtest_discover_tests(TestDatabase)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestDatabase)
