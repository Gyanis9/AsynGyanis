# Net 的 HTTP 报文解析依赖 llhttp，由本模块自行声明
find_package(llhttp REQUIRED)

add_library(Net
        TcpAcceptor.cpp
        TcpAcceptor.h
        TcpStream.cpp
        TcpStream.h
        TcpServer.cpp
        TcpServer.h
        HttpRequest.cpp
        HttpRequest.h
        HttpResponse.cpp
        HttpResponse.h
        HttpParser.cpp
        HttpParser.h
        HttpSession.cpp
        HttpSession.h
        Router.cpp
        Router.h
        HttpServer.cpp
        HttpServer.h
        HttpsSession.cpp
        HttpsSession.h
        HttpsServer.cpp
        HttpsServer.h
        InetAddress.h
        StreamBuffer.h
        Middleware.h
        FileSender.cpp
        FileSender.h
)

target_link_libraries(Net PUBLIC
        Core
        Base
        Platform
        llhttp::llhttp
)

if(TARGET jemalloc::jemalloc)
    target_link_libraries(Net PUBLIC jemalloc::jemalloc)
endif()

target_include_directories(Net PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)
