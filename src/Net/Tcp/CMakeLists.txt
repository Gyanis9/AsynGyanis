# Net/Tcp —— 监听套接字、带缓冲字节流与服务器基类
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/TcpAcceptor.h
        ${CMAKE_CURRENT_LIST_DIR}/TcpAcceptor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TcpStream.h
        ${CMAKE_CURRENT_LIST_DIR}/TcpStream.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TcpServer.h
        ${CMAKE_CURRENT_LIST_DIR}/TcpServer.cpp
)
