add_library(Core
        Epoll.cpp
        Epoll.h
        EpollAwaiter.h
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
)

target_include_directories(Core PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)
