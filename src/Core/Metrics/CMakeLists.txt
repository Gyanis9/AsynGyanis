# Core/Metrics —— 进程级读数的登记处：不进 HttpServerStats 的计数在这一层挂上，由 Net 那侧渲染导出
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/ProcessMetricsRegistry.h
        ${CMAKE_CURRENT_LIST_DIR}/ProcessMetricsRegistry.cpp
)
