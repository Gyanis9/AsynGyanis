# Net/Http3 —— HTTP/3 协议层（自研）：帧编解码、QPACK（含动态表）、连接级状态机与会话到既有 HTTP 层的映射。
# 库本身不再依赖第三方 h3 实现；nghttp3 只在测试里当跨实现裁判（见 tests/Net/CMakeLists.txt）。
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
        ${CMAKE_CURRENT_LIST_DIR}/Http3Session.h
        ${CMAKE_CURRENT_LIST_DIR}/Http3Session.cpp
)
