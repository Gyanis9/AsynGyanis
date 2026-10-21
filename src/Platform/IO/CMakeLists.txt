# Platform/IO —— 描述符、socket、事件通知、定时器与控制台交互
set_property(GLOBAL APPEND PROPERTY AsynGyanisPlatformSources
        ${CMAKE_CURRENT_LIST_DIR}/FileDescriptor.h
        ${CMAKE_CURRENT_LIST_DIR}/FileDescriptor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Socket.h
        ${CMAKE_CURRENT_LIST_DIR}/DatagramSocket.h
        ${CMAKE_CURRENT_LIST_DIR}/DatagramSocket.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Socket.cpp
        ${CMAKE_CURRENT_LIST_DIR}/NetworkInterface.h
        ${CMAKE_CURRENT_LIST_DIR}/NetworkInterface.cpp
        ${CMAKE_CURRENT_LIST_DIR}/EventNotifier.h
        ${CMAKE_CURRENT_LIST_DIR}/EventNotifier.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TimerFileDescriptor.h
        ${CMAKE_CURRENT_LIST_DIR}/TimerFileDescriptor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/MemoryMappedFile.h
        ${CMAKE_CURRENT_LIST_DIR}/MemoryMappedFile.cpp
        ${CMAKE_CURRENT_LIST_DIR}/FileContents.h
        ${CMAKE_CURRENT_LIST_DIR}/FileContents.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Console.h
        ${CMAKE_CURRENT_LIST_DIR}/Console.cpp
        ${CMAKE_CURRENT_LIST_DIR}/EventBackendContract.h
        ${CMAKE_CURRENT_LIST_DIR}/EventPoller.h
        ${CMAKE_CURRENT_LIST_DIR}/Epoll.h
)

# 事件后端按平台/构建开关三选一：Windows 是完成端口，Linux 默认 epoll，显式开关换 io_uring。
# Epoll.h 负责把 Epoll 这个名字指到选中的后端，EventPoller.h 再给 Core 一个与平台无关的别名。
if (WIN32)
    set_property(GLOBAL APPEND PROPERTY AsynGyanisPlatformSources
            ${CMAKE_CURRENT_LIST_DIR}/Iocp.cpp
            ${CMAKE_CURRENT_LIST_DIR}/Iocp.h
            ${CMAKE_CURRENT_LIST_DIR}/wepoll.h
    )
elseif (ASYN_WITH_IO_URING)
    set_property(GLOBAL APPEND PROPERTY AsynGyanisPlatformSources
            ${CMAKE_CURRENT_LIST_DIR}/Uring.cpp
            ${CMAKE_CURRENT_LIST_DIR}/Uring.h
    )
else ()
    set_property(GLOBAL APPEND PROPERTY AsynGyanisPlatformSources
            ${CMAKE_CURRENT_LIST_DIR}/Epoll.cpp
    )
endif ()
