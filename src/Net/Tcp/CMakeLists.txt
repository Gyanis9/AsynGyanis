# Net/Tcp —— 监听套接字、带缓冲字节流、服务器基类与按来源 IP 的连接限额
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/PerIpConnectionLimiter.h
        ${CMAKE_CURRENT_LIST_DIR}/PerIpConnectionLimiter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TcpAcceptor.h
        ${CMAKE_CURRENT_LIST_DIR}/TcpAcceptor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TcpClient.h
        ${CMAKE_CURRENT_LIST_DIR}/TcpClient.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TcpStream.h
        ${CMAKE_CURRENT_LIST_DIR}/TcpStream.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TcpServer.h
        ${CMAKE_CURRENT_LIST_DIR}/TcpServer.cpp
)
