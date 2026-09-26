# Net/Quic —— QUIC 传输层（全部自研）
# 字节层（Codec/）、包保护与 TLS 胶水（Crypto/）、组包器、连接状态机、恢复层（Recovery/）与
# 流层（Streams/）都在树内，本模块不链接任何第三方 QUIC 实现；跨实现验收走进程外的 aioquic
#（scripts/quic_cross_check.sh + tests/Tools/QuicProbeServer.cpp），树内不留裁判
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnectionRole.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicClientConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicClientConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicOpenSslError.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketBuilder.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketBuilder.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnectionCore.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnectionCore.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicReassemblyBuffer.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicReassemblyBuffer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicReceivedPacketNumbers.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicReceivedPacketNumbers.cpp
)

add_subdirectory(Codec)
add_subdirectory(Crypto)
add_subdirectory(Recovery)
add_subdirectory(Streams)
