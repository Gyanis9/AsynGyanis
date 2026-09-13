# Net/WebSocket —— RFC 6455：握手计算/校验、帧编解码与升级后的对端收发对象
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketHandshake.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketHandshake.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketFrame.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketFrame.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketPeer.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketPeer.cpp
)
