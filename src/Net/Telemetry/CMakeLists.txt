# Net/Telemetry —— OTLP 三个出口共用的内部支持（头文件只有一份）
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/OtlpHttpExportSupport.h
        ${CMAKE_CURRENT_LIST_DIR}/OtlpJsonSupport.h
        ${CMAKE_CURRENT_LIST_DIR}/OtlpConfigurationSupport.h
)
