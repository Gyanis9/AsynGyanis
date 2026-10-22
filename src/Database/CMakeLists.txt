# ============================================================================
# Database 模块 — 统一数据库抽象层与四种驱动实现
# ============================================================================
# Common  定义 DatabaseConnection / DatabaseResult / ConnectionConfig / 工厂契约
# Sqlite  嵌入式驱动，无需外部服务，故为必选依赖
# MySql / Postgres / Redis 依赖外部服务与第三方客户端库，按可选依赖处理：
#         Conan 能提供时编译真实驱动，缺失时编译返回明确错误的桩实现
# ============================================================================

find_package(SQLite3 REQUIRED)

# 登记随包导出的外部依赖（按组件记）：消费方链接 Database 时也要能找到 SQLite::SQLite3
set_property(GLOBAL APPEND PROPERTY AsynGyanisPackageDependencies_Database SQLite3)

# 可选驱动的统一接线：每个驱动都是「缓存或系统里有客户端包就编译真实实现，探测不到就
# 退化为每个入口都给中文错误的桩」。探测（QUIET + TARGET 判定，绝不 REQUIRED——缺一个
# 可选驱动不该让整个工程的 configure 失败）、链接、PUBLIC 宏、依赖登记与 status 五件事
# 收在一个函数里，新增驱动只需在下面加一行调用。
#
# 参数：driverName 是大写词干（同时是 option 名与宏名：option(DATABASE_WITH_<名>)、
# DATABASE_HAS_<名>）；cmakePackage 是 find_package 的名字（也是消费方要 find_dependency
# 的名字）；cmakeTarget 是它导出的目标；libraryName 是文案里点名的客户端库（用户要去装的
# 那个包名，PostgreSQL 上它与 find_package 名不同：包叫 libpq、配置叫 PostgreSQL）。
# 清单经 GLOBAL 属性给 cmake/AsynGyanisInstall.cmake 的能力清单段遍历——加驱动而没同步
# cmake/AsynGyanisFeatures.h.in 的宏，配置期当场拒绝而不是静默给 0。
set_property(GLOBAL PROPERTY AsynGyanisDatabaseDrivers "")
function(asyn_configure_database_driver driverName cmakePackage cmakeTarget libraryName displayName)
    set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseDrivers "${driverName}")
    option(DATABASE_WITH_${driverName} "编译 ${displayName} 驱动（需要可用的 ${libraryName}）" ON)
    if (DATABASE_WITH_${driverName})
        find_package(${cmakePackage} QUIET)
    endif ()

    if (TARGET ${cmakeTarget})
        target_link_libraries(Database PUBLIC ${cmakeTarget})
        target_compile_definitions(Database PUBLIC DATABASE_HAS_${driverName}=1)
        set_property(GLOBAL APPEND PROPERTY AsynGyanisPackageDependencies_Database ${cmakePackage})
        message(STATUS "Database: 启用 ${displayName} 驱动（${libraryName}）")
    elseif (DATABASE_WITH_${driverName})
        message(STATUS "Database: 未找到 ${libraryName}，${displayName} 驱动退化为报错桩（装上客户端库后重新 configure 即可启用）")
    else ()
        message(STATUS "Database: ${displayName} 驱动未启用（-DDATABASE_WITH_${driverName}=ON 可开启），退化为报错桩")
    endif ()

    # 能力清单的宏必须与这份清单同解：模板缺这一行时 configure_file 会静默落成 0，
    # 「库编了真驱动、清单说没有」要等到使用方在自己机器上分叉时才发现
    file(READ ${CMAKE_SOURCE_DIR}/cmake/AsynGyanisFeatures.h.in asynFeaturesTemplate)
    if (NOT asynFeaturesTemplate MATCHES "ASYN_HAS_${driverName}_DRIVER")
        message(FATAL_ERROR "cmake/AsynGyanisFeatures.h.in 缺 ASYN_HAS_${driverName}_DRIVER：新增驱动时能力清单要一并补上")
    endif ()
endfunction()

# 分层构建：叶子目录只登记本层源文件，逐层汇总后产出一个静态库
set_property(GLOBAL PROPERTY AsynGyanisDatabaseSources "")

add_subdirectory(Common)
add_subdirectory(Sqlite)
add_subdirectory(MySql)
add_subdirectory(Postgres)
add_subdirectory(Redis)
add_subdirectory(Pool)
add_subdirectory(Dialect)
add_subdirectory(Queryable)

get_property(DATABASE_SOURCES GLOBAL PROPERTY AsynGyanisDatabaseSources)

add_library(Database ${ASYN_LIBRARY_KIND}
        ${DATABASE_SOURCES}
)
# 静态是默认形态；打开 BUILD_SHARED_LIBS 时本模块要带上自己的导出宏定义与可见性预设
asyn_configure_module_library(Database)

if(MSVC)
    # Queryable/SchemaMigrator 的模板实例化会把单个消费方 TU 推到 MSVC 目标文件段数上限；
    # /bigobj 只放大段表容量，不改变代码生成，交给所有消费方而不是只给某一个测试
    target_compile_options(Database INTERFACE /bigobj)
endif()

# Base 是显式依赖：本模块的异常体系（DatabaseException 家族）派生自 Base::Exception，
# 虽然 Base 会经 Core 传递到本目标，但既然直接使用了它的类型就应当自己声明
target_link_libraries(Database PUBLIC
        Base
        Platform
        Core
        SQLite::SQLite3
)

# 三个可选驱动各一行：探测、链接、PUBLIC 宏、依赖登记与 status 全在函数里（见文件上部）。
# 目标名不是包名：PostgreSQL 包的 cmake_target_name 指向 PostgreSQL::PostgreSQL
#（Conan 侧组件引用写作 libpq::pq，见 packaging/conan/conanfile.py 的说明）
asyn_configure_database_driver(MYSQL libmysqlclient libmysqlclient::libmysqlclient libmysqlclient "MySQL")
asyn_configure_database_driver(POSTGRES PostgreSQL PostgreSQL::PostgreSQL libpq "PostgreSQL")
asyn_configure_database_driver(REDIS hiredis hiredis::hiredis hiredis "Redis")

target_include_directories(Database PUBLIC
        # 构建期用仓库内的 src；安装后改用 <前缀>/include，两者都是各模块头文件的根
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(Database PUBLIC cxx_std_23)
