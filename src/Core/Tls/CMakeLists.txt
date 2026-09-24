# Core/Tls —— 基于 OpenSSL 的 TLS 上下文与 TLS 套接字
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/SessionTicketKeyRing.h
        ${CMAKE_CURRENT_LIST_DIR}/SessionTicketKeyRing.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TlsContext.h
        ${CMAKE_CURRENT_LIST_DIR}/TlsContext.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TlsSocket.h
        ${CMAKE_CURRENT_LIST_DIR}/TlsSocket.cpp
)
