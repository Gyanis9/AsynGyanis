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

# wepoll 的**头文件**（epoll 的 Windows 兼容实现，BSD-2-Clause 许可）随仓库保留：
# 事件后端换成完成端口（Iocp）之后，Iocp.h 仍借它的 epoll_event 与事件位定义，因此头文件
# 随 Core 一起安装、不单独导出目标；AFD 轮询实现（Wepoll.cpp）已删除——事件通知全部走 Iocp.cpp，
# 留着两套 Windows 后端只会各自腐化
# 事件后端按平台二选一：Linux 是 epoll 的薄封装（Epoll.cpp），Windows 是完成端口后端（Iocp.cpp）。
# 两者接口同构，Epoll.h 在 Windows 上以 using Epoll = Iocp 指过去，因此上面那些文件不需要平台分支。
if (WIN32)
    set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
            ${CMAKE_CURRENT_LIST_DIR}/Iocp.cpp
            ${CMAKE_CURRENT_LIST_DIR}/Iocp.h
            ${CMAKE_CURRENT_LIST_DIR}/wepoll.h
    )
elseif (ASYN_WITH_IO_URING)
    # io_uring 后端（替代 epoll）：需要内核 5.6+，因此是显式开关而不是默认
    set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
            ${CMAKE_CURRENT_LIST_DIR}/Uring.cpp
            ${CMAKE_CURRENT_LIST_DIR}/Uring.h
    )
else ()
    set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
            ${CMAKE_CURRENT_LIST_DIR}/Epoll.cpp
    )
endif ()
