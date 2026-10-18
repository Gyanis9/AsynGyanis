# Net/WebSocket —— RFC 6455：握手计算/校验、帧编解码、文本负载 UTF-8 校验与升级后的对端收发对象
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/PerMessageDeflate.h
        ${CMAKE_CURRENT_LIST_DIR}/PerMessageDeflate.cpp
        # 只有头文件：WebSocket 客户端两段的失败类别与结果别名
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketError.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketHandshake.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketHandshake.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketUpgradePolicy.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketUpgradePolicy.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketFrame.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketFrame.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketPeer.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketPeer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketClient.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketClient.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketHub.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketHub.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketUtf8.h
        ${CMAKE_CURRENT_LIST_DIR}/WebSocketUtf8.cpp
)
