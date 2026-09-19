# Net/Quic —— QUIC 传输层
# 现状：自研的字节层（Codec/）、包保护与 TLS 胶水（Crypto/）、组包器与连接状态机都在树内；
# 生产路径仍走 ngtcp2（见 third_party/），等状态机在真机上打通后再换，两套并存是刻意的过渡态
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicOpenSslError.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketBuilder.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketBuilder.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnectionCore.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnectionCore.cpp
)

add_subdirectory(Codec)
add_subdirectory(Crypto)
add_subdirectory(Recovery)
