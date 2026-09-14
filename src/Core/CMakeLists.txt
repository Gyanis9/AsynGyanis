# Core 的 TLS 封装所需依赖，由本模块自行声明
find_package(OpenSSL REQUIRED)

# 登记随包导出的外部依赖（按组件记）：消费方链接 Core 时也要能找到 OpenSSL::SSL / OpenSSL::Crypto
set_property(GLOBAL APPEND PROPERTY AsynGyanisPackageDependencies_Core OpenSSL)

# Core 分层构建：叶子目录只登记本层源文件，逐层汇总后产出一个静态库
set_property(GLOBAL PROPERTY AsynGyanisCoreSources "")

add_subdirectory(Exception)
add_subdirectory(EventLoop)
add_subdirectory(Coroutine)
add_subdirectory(Socket)
add_subdirectory(Tls)

get_property(CORE_SOURCES GLOBAL PROPERTY AsynGyanisCoreSources)

add_library(Core STATIC
        ${CORE_SOURCES}
)

target_link_libraries(Core PUBLIC
        Base
        Platform
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(ASYN_WITH_MIMALLOC)
    # 全局分配器接管整个进程的 malloc/free（分配器随 Core 进入最终可执行文件）：
    # 由 Conan 提供，Windows/Linux 都可用；开关与 sanitizer 的互斥在顶层 CMakeLists 已拦住。
    # 注意目标是 ConanCenter 配方声明的无命名空间名 mimalloc-static（实机核对过生成产物）
    find_package(mimalloc REQUIRED)
    target_link_libraries(Core PUBLIC mimalloc-static)
    # 启用后消费方同样要能找到分配器目标
    set_property(GLOBAL APPEND PROPERTY AsynGyanisPackageDependencies_Core mimalloc)
endif()

target_include_directories(Core PUBLIC
        # 构建期用仓库内的 src；安装后改用 <前缀>/include，两者都是各模块头文件的根
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(Core PUBLIC cxx_std_20)
