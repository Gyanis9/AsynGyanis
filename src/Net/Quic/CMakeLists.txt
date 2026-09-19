# Net/Quic —— QUIC 传输层
# 现状：状态机与 TLS 胶水仍由 ngtcp2 提供（见 third_party/），Codec/ 是自研的无状态字节层。
# 两层并存是刻意的过渡态：字节层不依赖任何跨包状态，可以先独立测准
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.cpp
)

add_subdirectory(Codec)
add_subdirectory(Crypto)
