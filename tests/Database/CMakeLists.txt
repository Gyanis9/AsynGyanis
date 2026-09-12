find_package(GTest REQUIRED)

# 测试目录结构与 src/Database 逐级对应，全部链接进同一个 TestDatabase 可执行文件
add_executable(TestDatabase
        DatabaseTestSupport.h

        Common/TestDatabaseTypes.cpp
        Common/TestDatabaseFactory.cpp

        Pool/TestConnectionPool.cpp
        Pool/TestTransaction.cpp

        Dialect/TestSqliteDialect.cpp

        Queryable/TestQueryable.cpp
        Queryable/TestQueryableExecution.cpp
        Queryable/TestParameterizedExecution.cpp

        Sqlite/TestSqliteConnection.cpp
        Sqlite/TestSqliteResult.cpp

        MySql/TestMySqlConnection.cpp

        Redis/TestRedisConnection.cpp
        Redis/TestRedisResult.cpp
)

target_link_libraries(TestDatabase PRIVATE
        Database
        GTest::gtest
        GTest::gtest_main
)

# src 用于 "Database/..." 形式的生产头包含，本目录用于 "DatabaseTestSupport.h" 同级包含
target_include_directories(TestDatabase PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(TestDatabase PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(TestDatabase)
