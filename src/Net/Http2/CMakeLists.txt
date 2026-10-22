# Net/Http2 —— RFC 9113/7541：帧编解码、增量解码器、HPACK 头块压缩、连接状态机与会话
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/Http2ErrorOut.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2Frame.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2Frame.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Hpack.h
        ${CMAKE_CURRENT_LIST_DIR}/Hpack.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http2Connection.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2Connection.cpp
        # 入站帧分派与逐类帧处理
        ${CMAKE_CURRENT_LIST_DIR}/Http2ConnectionFrames.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http2Session.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2Session.cpp
        # 响应头组装、响应写出与流式收尾；共用头只给这两份翻译单元
        ${CMAKE_CURRENT_LIST_DIR}/Http2SessionSupport.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2SessionResponses.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http2ClientConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/Http2ClientConnection.cpp
)
