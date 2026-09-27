# Net/Tracing —— 链路节与编排器：采样、攒批、经出口送出去（OTLP/HTTP 与文件两种出口）
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/Span.h
        ${CMAKE_CURRENT_LIST_DIR}/Span.cpp
        ${CMAKE_CURRENT_LIST_DIR}/SpanExporter.h
        ${CMAKE_CURRENT_LIST_DIR}/Tracer.h
        ${CMAKE_CURRENT_LIST_DIR}/Tracer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/OtlpTraceJson.h
        ${CMAKE_CURRENT_LIST_DIR}/OtlpTraceJson.cpp
        ${CMAKE_CURRENT_LIST_DIR}/FileSpanExporter.h
        ${CMAKE_CURRENT_LIST_DIR}/FileSpanExporter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/OtlpHttpSpanExporter.h
        ${CMAKE_CURRENT_LIST_DIR}/OtlpHttpSpanExporter.cpp
)
