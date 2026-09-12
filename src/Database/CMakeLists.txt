# ============================================================================
# Database 模块 — 统一数据库抽象层与三种驱动实现
# ============================================================================
# Common  定义 DatabaseConnection / DatabaseResult / ConnectionConfig / 工厂契约
# Sqlite  嵌入式驱动，无需外部服务，故为必选依赖
# MySql / Redis 依赖外部服务与第三方客户端库，按可选依赖处理：
#         Conan 能提供时编译真实驱动，缺失时编译返回明确错误的桩实现
# ============================================================================

find_package(SQLite3 REQUIRED)
find_package(hiredis QUIET)

# libmysqlclient 是可选依赖：Conan 缓存或系统里存在匹配本 profile 的包时编译真实驱动，
# 探测不到就退化为「每个入口都给出中文错误」的桩实现。因此这里用 QUIET 探测 +
# if(TARGET ...) 判定，绝不写 find_package(... REQUIRED)：
# 缺一个可选驱动不该让整个工程的 configure 失败（旧的 OPTION 默认 OFF 写法让真实驱动
# 从未被编译过，真实分支里的编译错误也就无从发现）
option(DATABASE_WITH_MYSQL "编译 MySQL 驱动（需要可用的 libmysqlclient）" ON)

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

# Base 是显式依赖：本模块的异常体系（DatabaseException 家族）派生自 Base::Exception，
# 虽然 Base 会经 Core 传递到本目标，但既然直接使用了它的类型就应当自己声明
target_link_libraries(Database PUBLIC
        Base
        Platform
        Core
        SQLite::SQLite3
)

# 可选驱动只在启用时才探测客户端库，并用对应宏切换真实实现与报错桩。
# QUIET + TARGET 判定：探测失败是预期路径（没装客户端库的机器），不打印红色告警也不中断
if(DATABASE_WITH_MYSQL)
    find_package(libmysqlclient QUIET)
endif()

if(TARGET libmysqlclient::libmysqlclient)
    target_link_libraries(Database PUBLIC libmysqlclient::libmysqlclient)
    target_compile_definitions(Database PUBLIC DATABASE_HAS_MYSQL=1)
    message(STATUS "Database: 启用 MySQL 驱动（libmysqlclient）")
elseif(DATABASE_WITH_MYSQL)
    message(STATUS "Database: 未找到 libmysqlclient，MySQL 驱动退化为报错桩（装上客户端库后重新 configure 即可启用）")
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
