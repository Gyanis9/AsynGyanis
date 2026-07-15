# ============================================================================
# Database 模块 — 统一数据库抽象层
# ============================================================================
# 提供 MySQL、Redis 等数据库的统一访问接口，屏蔽底层驱动的方言差异。
#
# 依赖（按需启用）：
#   - MySQL 封装需要 libmysqlclient（通过 Conan 安装 mysql-connector-c）
#   - Redis 封装需要 hiredis（通过 Conan 安装 hiredis）
# ============================================================================

# 公共头文件（始终编译）
set(DATABASE_PUBLIC_HEADERS
    DatabaseType.h
    DatabaseResult.h
    DatabaseConnection.h
    DatabaseFactory.h
)

# MySQL 实现
set(DATABASE_MYSQL_SOURCES
    MySqlConnection.cpp
    MySqlConnection.h
    MySqlResult.cpp
    MySqlResult.h
)

# Redis 实现
set(DATABASE_REDIS_SOURCES
    RedisConnection.cpp
    RedisConnection.h
    RedisResult.cpp
    RedisResult.h
)

# SQLite 实现
set(DATABASE_SQLITE_SOURCES
    SqliteConnection.cpp
    SqliteConnection.h
    SqliteResult.cpp
    SqliteResult.h
)

# 组合完整源文件列表
set(DATABASE_SOURCES
    ${DATABASE_PUBLIC_HEADERS}
    ${DATABASE_MYSQL_SOURCES}
    ${DATABASE_REDIS_SOURCES}
    ${DATABASE_SQLITE_SOURCES}
    DatabaseFactory.cpp
)

add_library(Database STATIC ${DATABASE_SOURCES})

target_include_directories(Database PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Database PUBLIC Base)

# 设置 C++ 标准
target_compile_features(Database PUBLIC cxx_std_20)

# ============================================================================
# 数据库驱动依赖
# ============================================================================

# SQLite（始终启用，sqlite3 为嵌入式库无外部依赖）
find_package(SQLite3 REQUIRED)
target_link_libraries(Database PRIVATE SQLite::SQLite3)
target_compile_definitions(Database PRIVATE DATABASE_HAS_SQLITE)

# MySQL 支持（可选，需要系统安装 libmysqlclient 或 mysql-connector-c）
find_package(libmysqlclient QUIET)
if(libmysqlclient_FOUND)
    target_link_libraries(Database PRIVATE libmysqlclient::libmysqlclient)
    target_compile_definitions(Database PRIVATE DATABASE_HAS_MYSQL)
    message(STATUS "Database: MySQL 支持已启用")
else()
    message(STATUS "Database: MySQL 支持未启用（libmysqlclient 未找到）")
endif()

# Redis 支持（可选，需要系统安装 hiredis）
find_package(hiredis QUIET)
if(hiredis_FOUND)
    target_link_libraries(Database PRIVATE hiredis::hiredis)
    target_compile_definitions(Database PRIVATE DATABASE_HAS_REDIS)
    message(STATUS "Database: Redis 支持已启用")
else()
    message(STATUS "Database: Redis 支持未启用（hiredis 未找到）")
endif()
