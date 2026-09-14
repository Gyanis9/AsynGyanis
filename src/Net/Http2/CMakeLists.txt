# Net/Http2 —— RFC 7540/7541：帧头与各类型帧负载的编解码、增量帧解码器、HPACK 头块压缩、连接层状态机与会话
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/Http2Frame.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2Frame.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Hpack.h
        ${CMAKE_CURRENT_LIST_DIR}/Hpack.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http2StreamBody.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2StreamBody.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http2Connection.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2Connection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http2Session.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2Session.cpp
)
