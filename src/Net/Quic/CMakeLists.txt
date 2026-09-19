# Net/Quic —— QUIC 传输层（全部自研）
# 字节层（Codec/）、包保护与 TLS 胶水（Crypto/）、组包器、连接状态机、恢复层（Recovery/）与
# 流层（Streams/）都在树内，Net 不链接 ngtcp2；那份第三方实现只剩测试里的跨实现裁判
#（登记在 tests/Net/CMakeLists.txt，与 nghttp3 同一个道理）
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
        ${CMAKE_CURRENT_LIST_DIR}/QuicReassemblyBuffer.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicReassemblyBuffer.cpp
)

add_subdirectory(Codec)
add_subdirectory(Crypto)
add_subdirectory(Recovery)
add_subdirectory(Streams)
