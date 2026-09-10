# Core 的 TLS 与 epoll 封装所需依赖，由本模块自行声明
find_package(OpenSSL REQUIRED)

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

add_library(Core
        Epoll.cpp
        Epoll.h
        EpollAwaiter.h
        TlsSocket.cpp
        TlsSocket.h
        TlsContext.cpp
        TlsContext.h
        Task.h
        Scheduler.cpp
        Scheduler.h
        CoroutinePool.cpp
        CoroutinePool.h
        ThreadPool.cpp
        ThreadPool.h
        EventLoop.cpp
        EventLoop.h
        IoContext.cpp
        IoContext.h
        Timer.cpp
        Timer.h
        InetAddress.cpp
        InetAddress.h
        AsyncSocket.cpp
        AsyncSocket.h
        BufferPool.cpp
        BufferPool.h
        Cancelable.h
        Connection.cpp
        Connection.h
        ConnectionManager.cpp
        ConnectionManager.h
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

if(WIN32)
    target_link_libraries(Core PUBLIC wepoll)
endif()

target_include_directories(Core PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)
