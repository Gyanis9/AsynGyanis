# Net/Logging —— 日志的 OTLP/HTTP 出口与它的配置段
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/OtlpLogJson.h
        ${CMAKE_CURRENT_LIST_DIR}/OtlpLogJson.cpp
        ${CMAKE_CURRENT_LIST_DIR}/OtlpLogSink.h
        ${CMAKE_CURRENT_LIST_DIR}/OtlpLogSink.cpp
        ${CMAKE_CURRENT_LIST_DIR}/LogExportConfiguration.h
        ${CMAKE_CURRENT_LIST_DIR}/LogExportConfiguration.cpp
)
