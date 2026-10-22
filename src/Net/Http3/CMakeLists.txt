# Net/Http3 —— HTTP/3 协议层（自研）：帧编解码、QPACK（含动态表）、连接状态机与会话映射。
# 库与测试对端都不依赖第三方 h3 实现，字节级一致性验收放在进程外。
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/Http3Error.h
        ${CMAKE_CURRENT_LIST_DIR}/Http3Error.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http3Frame.h
        ${CMAKE_CURRENT_LIST_DIR}/Http3Frame.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http3HeaderValidation.h
        ${CMAKE_CURRENT_LIST_DIR}/Http3HeaderValidation.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QpackStaticTable.h
        ${CMAKE_CURRENT_LIST_DIR}/Qpack.h
        ${CMAKE_CURRENT_LIST_DIR}/Qpack.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http3Connection.h
        ${CMAKE_CURRENT_LIST_DIR}/Http3Connection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http3ClientConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/Http3ClientConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http3Session.h
        ${CMAKE_CURRENT_LIST_DIR}/Http3Session.cpp
        # 整段响应、流式响应与收尾
        ${CMAKE_CURRENT_LIST_DIR}/Http3SessionResponses.cpp
)
