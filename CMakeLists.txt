cmake_minimum_required(VERSION 3.20)

project(AsynGyanis
        VERSION 1.1.0
        DESCRIPTION "Industrial-grade C++23 async server framework"
        LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 目标平台只有 Linux 与 Windows：事件后端是 epoll 与完成端口，没有 kqueue。
# 这是拒绝文案而不是待办：不要为 macOS/BSD 补后端。
if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
            "AsynGyanis 目前只支持 Linux 与 Windows：事件循环的实现是 epoll 与完成端口，"
            "没有可用的 kqueue 后端（当前系统：${CMAKE_SYSTEM_NAME}）。要支持该系统，"
            "需要新增一个与 Core/EventLoop/Epoll.h 同接口的事件通知实现，并在 Core 的 CMakeLists 里按平台选用。")
endif()

include(GNUInstallDirs)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/AsynGyanisLibrary.cmake)

# MSVC 强制 UTF-8 源码编码。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Release 也带 PDB：异常与日志的调用栈需要符号；/OPT:REF,ICF 必须显式写回。
if(MSVC)
    # 不用 CMAKE_MSVC_DEBUG_INFORMATION_FORMAT：CMP0141 在 3.20 下限停在 OLD，变量会被忽略。
    # 调用方显式点 Embedded 时让位：CI 并行 Release 用 /Zi 共享 PDB 会撞 C1041。
    if(NOT CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
        add_compile_options($<$<CONFIG:Release>:/Zi>)
    endif()
    # 给 /DEBUG 会关掉默认的 /OPT:REF,ICF；实测 h1 解析 730→961 ns，必须显式写回。
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
else()
    # GCC/Clang 的栈回溯靠 -g 给出函数名与行号。
    add_compile_options($<$<CONFIG:Release>:-g>)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/AsynGyanisOptions.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/AsynGyanisSanitizers.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/AsynGyanisTargetFlags.cmake)

# Debug 注入 ASYN_DEBUG：日志输出线程号与源码位置。
add_compile_definitions($<$<CONFIG:Debug>:ASYN_DEBUG>)

enable_testing()

# Windows DLL 与可执行体必须同目录，否则 Ninja 下启动是 0xC0000135。
# Linux 的 .so 走 RPATH，静态形态不进这个分支。
if(BUILD_SHARED_LIBS AND WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

add_subdirectory(src)

if(ASYN_BUILD_TESTS)
    add_subdirectory(tests)
    add_subdirectory(benchmarks/microbench)
endif()

if(ASYN_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

asyn_apply_project_target_flags()
include(${CMAKE_CURRENT_LIST_DIR}/cmake/AsynGyanisInstall.cmake)
