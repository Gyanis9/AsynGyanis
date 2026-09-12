# ============================================================================
# Database 模块 — 统一数据库抽象层与三种驱动实现
# ============================================================================
# Common  定义 DatabaseConnection / DatabaseResult / ConnectionConfig / 工厂契约
# Sqlite  嵌入式驱动，无需外部服务，故为必选依赖
# MySql   与 Redis 依赖外部服务与第三方客户端库，按可选依赖处理：
#         Conan 能提供时编译真实驱动，缺失时编译返回明确错误的桩实现
# ============================================================================

find_package(SQLite3 REQUIRED)
find_package(hiredis QUIET)

# libmysqlclient 在 Conan 缓存里只有源码包、没有匹配本 profile 的预编译二进制，
# 自动探测会生成指向不存在头文件目录的配置，最终以「找不到 mysql/mysql.h」失败。
# 因此 MySQL 驱动默认关闭，准备好客户端库后用 -DDATABASE_WITH_MYSQL=ON 显式开启
option(DATABASE_WITH_MYSQL "编译 MySQL 驱动（需要可用的 libmysqlclient）" OFF)

# 分层构建：叶子目录只登记本层源文件，逐层汇总后产出一个静态库
set_property(GLOBAL PROPERTY AsynGyanisDatabaseSources "")

add_subdirectory(Common)
add_subdirectory(Sqlite)
add_subdirectory(MySql)
add_subdirectory(Redis)
add_subdirectory(Pool)
add_subdirectory(Dialect)
add_subdirectory(Queryable)

get_property(DATABASE_SOURCES GLOBAL PROPERTY AsynGyanisDatabaseSources)

add_library(Database STATIC
        ${DATABASE_SOURCES}
)

target_link_libraries(Database PUBLIC
        Platform
        Core
        SQLite::SQLite3
)

# 可选驱动只在启用时才探测客户端库，并用对应宏切换真实实现与报错桩
if(DATABASE_WITH_MYSQL)
    find_package(libmysqlclient REQUIRED)
    target_link_libraries(Database PUBLIC libmysqlclient::libmysqlclient)
    target_compile_definitions(Database PUBLIC DATABASE_HAS_MYSQL=1)
    message(STATUS "Database: 启用 MySQL 驱动（libmysqlclient）")
else()
    message(STATUS "Database: MySQL 驱动未启用（-DDATABASE_WITH_MYSQL=ON 可开启），退化为报错桩")
endif()

if(TARGET hiredis::hiredis)
    target_link_libraries(Database PUBLIC hiredis::hiredis)
    target_compile_definitions(Database PUBLIC DATABASE_HAS_REDIS=1)
    message(STATUS "Database: 启用 Redis 驱动（hiredis）")
else()
    message(STATUS "Database: 未找到 hiredis，Redis 驱动退化为报错桩")
endif()

target_include_directories(Database PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(Database PUBLIC cxx_std_20)
