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
)

target_link_libraries(Net PUBLIC
        Core
        Base
        llhttp::llhttp
)

target_include_directories(Net PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)
