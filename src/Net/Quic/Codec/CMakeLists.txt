# Net/Quic/Codec —— QUIC 报文编解码（RFC 9000 §16/§17）：变长整数、报文头与帧的无状态字节层
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/QuicDecodeError.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicVariableLengthInteger.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicVariableLengthInteger.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketHeader.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketHeader.cpp
)
