# Net/Quic —— QUIC 传输层（全部自研）：字节层、包保护、组包器、连接状态机、恢复层与流层都在树内，
# 不链接任何第三方 QUIC；跨实现验收走进程外的 aioquic（scripts/quic_cross_check.sh +
# tests/Tools/QuicProbeServer.cpp）。
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/QuicSupport.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnectionRole.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicClientConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicClientConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicServer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicOpenSslError.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicOpenSslError.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketBuilder.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketBuilder.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicAddressValidationToken.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicAddressValidationToken.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnectionCore.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnectionCore.cpp
        # 对端路径跟随、挑战记账与 PATH_RESPONSE 处理
        ${CMAKE_CURRENT_LIST_DIR}/QuicConnectionCorePaths.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicOutboundDatagram.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicReassemblyBuffer.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicReassemblyBuffer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicReceivedPacketNumbers.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicReceivedPacketNumbers.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicPathMtuDiscovery.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicPathMtuDiscovery.cpp
)

add_subdirectory(Codec)
add_subdirectory(Crypto)
add_subdirectory(Recovery)
add_subdirectory(Streams)
