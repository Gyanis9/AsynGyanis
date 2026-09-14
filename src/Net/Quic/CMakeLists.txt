# Net/Quic —— QUIC 传输层（ngtcp2 + nghttp3 + OpenSSL 的 QUIC TLS 回调）
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.cpp
)
