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

# Debug 构建注入 ASYN_DEBUG：日志输出线程号与源码位置，Release 走精简格式
add_compile_definitions($<$<CONFIG:Debug>:ASYN_DEBUG>)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)