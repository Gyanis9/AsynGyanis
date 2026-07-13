cmake_minimum_required(VERSION 3.20)

project(AsynGyanis
        VERSION 1.0.0
        DESCRIPTION "Industrial-grade C++20 async server framework"
        LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: 强制 UTF-8 源码编码，解决中文注释和字符串在 GBK 环境下编译失败的问题
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(OpenSSL REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Catch2 REQUIRED)
find_package(llhttp REQUIRED)

# jemalloc 在 Windows 上可能不可用，设为可选
find_package(jemalloc QUIET)

# Windows: 获取 wepoll（基于 IOCP 的 epoll 兼容层）
if(WIN32)
    set(WEPOLL_DIR "${CMAKE_BINARY_DIR}/wepoll")
    file(MAKE_DIRECTORY ${WEPOLL_DIR})

    if(NOT EXISTS "${WEPOLL_DIR}/wepoll.h")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/piscisaureus/wepoll/v1.5.7/wepoll.h"
            "${WEPOLL_DIR}/wepoll.h"
            STATUS wepoll_h_status)
    endif()

    if(NOT EXISTS "${WEPOLL_DIR}/wepoll.c")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/piscisaureus/wepoll/v1.5.7/wepoll.c"
            "${WEPOLL_DIR}/wepoll.c"
            STATUS wepoll_c_status)
    endif()

    add_library(wepoll STATIC ${WEPOLL_DIR}/wepoll.c)
    target_include_directories(wepoll PUBLIC ${WEPOLL_DIR})
endif()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
add_subdirectory(samples)