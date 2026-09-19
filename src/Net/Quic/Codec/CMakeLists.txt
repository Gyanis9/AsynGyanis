# Net/Quic/Codec —— QUIC 报文编解码（RFC 9000 §16–§18）：变长整数、报文头、帧与传输参数的无状态字节层
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/QuicDecodeError.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicRawBytes.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicVariableLengthInteger.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicVariableLengthInteger.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketHeader.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicPacketHeader.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicFrame.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicFrame.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QuicTransportParameters.h
        ${CMAKE_CURRENT_LIST_DIR}/QuicTransportParameters.cpp
)
