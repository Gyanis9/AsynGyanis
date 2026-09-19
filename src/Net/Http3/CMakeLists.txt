# Net/Http3 —— HTTP/3 协议层（nghttp3）：控制流与 QPACK 流的绑定、h3 帧与既有 HTTP 层的映射
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/Http3Error.h
        ${CMAKE_CURRENT_LIST_DIR}/Http3Error.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Http3Session.h
        ${CMAKE_CURRENT_LIST_DIR}/Http3Session.cpp
)
