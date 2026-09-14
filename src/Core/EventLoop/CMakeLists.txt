# Core/EventLoop —— 事件循环、跨平台 epoll 封装与定时器
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/EventLoop.h
        ${CMAKE_CURRENT_LIST_DIR}/EventLoop.cpp
        ${CMAKE_CURRENT_LIST_DIR}/IoContext.h
        ${CMAKE_CURRENT_LIST_DIR}/IoContext.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionDistributor.h
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionDistributor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Epoll.h
        ${CMAKE_CURRENT_LIST_DIR}/IoWatcher.h
        ${CMAKE_CURRENT_LIST_DIR}/IoWatcher.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Timer.h
        ${CMAKE_CURRENT_LIST_DIR}/Timer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TimerQueue.h
        ${CMAKE_CURRENT_LIST_DIR}/TimerQueue.cpp
)

# wepoll（epoll 的 Windows 兼容实现，BSD-2-Clause 许可，见 wepoll.h 头部）已随仓库 vendored，
# 不再构建期联网下载。它**编进 Core 库本身**、不单独导出目标：Windows 侧的事件通知是 Core 的
# 实现细节，消费方只需链 AsynGyanis::Core，不必知道底下是谁在做。
# wepoll.h 与 Wepoll.cpp 同目录：Epoll.h 用引号包含 "wepoll.h"，引号包含先从包含者所在目录找
# 事件后端按平台二选一：Linux 是 epoll 的薄封装（Epoll.cpp），Windows 是完成端口后端（Iocp.cpp）。
# 两者接口同构，Epoll.h 在 Windows 上以 using Epoll = Iocp 指过去，因此上面那些文件不需要平台分支。
if (WIN32)
    set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
            ${CMAKE_CURRENT_LIST_DIR}/Iocp.cpp
            ${CMAKE_CURRENT_LIST_DIR}/Iocp.h
            ${CMAKE_CURRENT_LIST_DIR}/Wepoll.cpp
            ${CMAKE_CURRENT_LIST_DIR}/wepoll.h
    )
else ()
    set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
            ${CMAKE_CURRENT_LIST_DIR}/Epoll.cpp
    )
endif ()
