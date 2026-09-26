# Core/Socket —— 异步套接字、地址与连接管理
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/AsyncResolver.h
        ${CMAKE_CURRENT_LIST_DIR}/AsyncResolver.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AsyncSocket.h
        ${CMAKE_CURRENT_LIST_DIR}/AsyncUdpSocket.h
        ${CMAKE_CURRENT_LIST_DIR}/AsyncUdpSocket.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AsyncSocket.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionRace.h
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionRace.cpp
        ${CMAKE_CURRENT_LIST_DIR}/VectoredSendCursor.h
        ${CMAKE_CURRENT_LIST_DIR}/VectoredSendCursor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/InetAddress.h
        ${CMAKE_CURRENT_LIST_DIR}/InetAddress.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Connection.h
        ${CMAKE_CURRENT_LIST_DIR}/Connection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionManager.h
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionManager.cpp
)
