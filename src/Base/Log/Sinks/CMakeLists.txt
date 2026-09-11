# Base/Log/Sinks —— 日志输出目标：抽象基类与控制台/文件/滚动/异步四类实现
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/LogSink.h
        ${CMAKE_CURRENT_LIST_DIR}/LogSink.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConsoleSink.h
        ${CMAKE_CURRENT_LIST_DIR}/ConsoleSink.cpp
        ${CMAKE_CURRENT_LIST_DIR}/FileSink.h
        ${CMAKE_CURRENT_LIST_DIR}/FileSink.cpp
        ${CMAKE_CURRENT_LIST_DIR}/RollingFileSink.h
        ${CMAKE_CURRENT_LIST_DIR}/RollingFileSink.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AsyncSink.h
        ${CMAKE_CURRENT_LIST_DIR}/AsyncSink.cpp
)
