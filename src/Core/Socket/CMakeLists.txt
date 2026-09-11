# Core/Socket —— 异步套接字、地址、缓冲池与连接管理
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/AsyncSocket.h
        ${CMAKE_CURRENT_LIST_DIR}/AsyncSocket.cpp
        ${CMAKE_CURRENT_LIST_DIR}/InetAddress.h
        ${CMAKE_CURRENT_LIST_DIR}/InetAddress.cpp
        ${CMAKE_CURRENT_LIST_DIR}/BufferPool.h
        ${CMAKE_CURRENT_LIST_DIR}/BufferPool.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Connection.h
        ${CMAKE_CURRENT_LIST_DIR}/Connection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionManager.h
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionManager.cpp
)
