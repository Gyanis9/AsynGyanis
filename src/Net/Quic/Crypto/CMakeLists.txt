# Net/Quic/Crypto —— QUIC 密钥与报文保护（RFC 9001 §5）：密钥表、头部保护与 AEAD 收发
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketKeys.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketKeys.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicKeySchedule.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicKeySchedule.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicHeaderProtection.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicHeaderProtection.cpp
)
