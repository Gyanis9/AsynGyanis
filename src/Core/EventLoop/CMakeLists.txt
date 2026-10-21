# Core/EventLoop —— 事件循环、派发与定时器
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/EventLoop.h
        ${CMAKE_CURRENT_LIST_DIR}/EventLoop.cpp
        ${CMAKE_CURRENT_LIST_DIR}/IoContext.h
        ${CMAKE_CURRENT_LIST_DIR}/IoContext.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionDistributor.h
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionDistributor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/IoWatcher.h
        ${CMAKE_CURRENT_LIST_DIR}/IoWatcher.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Timer.h
        ${CMAKE_CURRENT_LIST_DIR}/Timer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TimerQueue.h
        ${CMAKE_CURRENT_LIST_DIR}/TimerQueue.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WakeupFailureReport.h
)

# 事件后端（epoll/Iocp/Uring）与契约在 Platform/IO：Core 只消费 Platform::EventPoller。
