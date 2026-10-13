# Net/Metrics —— 进程读数导出：把注册表里的读数按周期推给 OTLP/HTTP 采集端
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/OtlpMetricsJson.h
        ${CMAKE_CURRENT_LIST_DIR}/OtlpMetricsJson.cpp
        ${CMAKE_CURRENT_LIST_DIR}/OtlpHttpMetricsExporter.h
        ${CMAKE_CURRENT_LIST_DIR}/OtlpHttpMetricsExporter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/MetricsExportConfiguration.h
        ${CMAKE_CURRENT_LIST_DIR}/MetricsExportConfiguration.cpp
        ${CMAKE_CURRENT_LIST_DIR}/HttpStatsOtlp.h
        ${CMAKE_CURRENT_LIST_DIR}/HttpStatsOtlp.cpp
)
