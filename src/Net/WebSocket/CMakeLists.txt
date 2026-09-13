# Net/WebSocket —— RFC 6455：握手计算/校验与帧编解码（尚未接入会话）
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketHandshake.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketHandshake.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketFrame.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketFrame.cpp
)
