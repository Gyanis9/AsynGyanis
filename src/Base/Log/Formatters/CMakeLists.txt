# Base/Log/Formatters —— 日志事件到文本的格式化器
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/LogFormatter.h
        ${CMAKE_CURRENT_LIST_DIR}/DefaultFormatter.h
        ${CMAKE_CURRENT_LIST_DIR}/DefaultFormatter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ColorFormatter.h
        ${CMAKE_CURRENT_LIST_DIR}/ColorFormatter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/JsonFormatter.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonFormatter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/PaddedFieldText.h
        ${CMAKE_CURRENT_LIST_DIR}/SourceLocationText.h
        ${CMAKE_CURRENT_LIST_DIR}/StackTraceText.h
        ${CMAKE_CURRENT_LIST_DIR}/TimestampText.h
)
