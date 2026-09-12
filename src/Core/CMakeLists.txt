# Core 的 TLS 封装所需依赖，由本模块自行声明
find_package(OpenSSL REQUIRED)

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
endif()

# wepoll 目标在 EventLoop/CMakeLists.txt 中按平台定义
if(WIN32)
    target_link_libraries(Core PUBLIC wepoll)
endif()

target_include_directories(Core PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(Core PUBLIC cxx_std_20)
