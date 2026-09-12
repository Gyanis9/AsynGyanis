# Core/EventLoop —— 事件循环、跨平台 epoll 封装与定时器
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/EventLoop.h
        ${CMAKE_CURRENT_LIST_DIR}/EventLoop.cpp
        ${CMAKE_CURRENT_LIST_DIR}/IoContext.h
        ${CMAKE_CURRENT_LIST_DIR}/IoContext.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Epoll.h
        ${CMAKE_CURRENT_LIST_DIR}/Epoll.cpp
        ${CMAKE_CURRENT_LIST_DIR}/IoWatcher.h
        ${CMAKE_CURRENT_LIST_DIR}/IoWatcher.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Timer.h
        ${CMAKE_CURRENT_LIST_DIR}/Timer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TimerQueue.h
        ${CMAKE_CURRENT_LIST_DIR}/TimerQueue.cpp
)

# wepoll（epoll 的 Windows 兼容实现，BSD-2-Clause 许可，见 wepoll.h 头部）已随仓库 vendored，
# 不再构建期联网下载；仅在 Windows 上编译并供 Epoll 封装使用
if (WIN32)
    add_library(wepoll STATIC
            ${CMAKE_CURRENT_LIST_DIR}/wepoll/wepoll.c
            ${CMAKE_CURRENT_LIST_DIR}/wepoll/wepoll.h
    )
    target_include_directories(wepoll PUBLIC ${CMAKE_CURRENT_LIST_DIR}/wepoll)
endif ()
