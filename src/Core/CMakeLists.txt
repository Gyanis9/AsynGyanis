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

if(TARGET jemalloc::jemalloc)
    target_link_libraries(Core PUBLIC jemalloc::jemalloc)
    # 启用后可执行文件多一层分配器依赖，消费方同样要能找到它
    set_property(GLOBAL APPEND PROPERTY AsynGyanisPackageDependencies_Core jemalloc)
endif()

target_include_directories(Core PUBLIC
        # 构建期用仓库内的 src；安装后改用 <前缀>/include，两者都是各模块头文件的根
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(Core PUBLIC cxx_std_20)
