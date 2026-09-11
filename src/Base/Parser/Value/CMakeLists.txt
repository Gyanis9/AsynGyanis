# Base/Parser/Value —— 跨格式共用的文档值模型与其错误类型
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/ParserValueType.h
        ${CMAKE_CURRENT_LIST_DIR}/ParserValueType.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ValueAccessError.h
        ${CMAKE_CURRENT_LIST_DIR}/ValueAccessError.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ParserValue.h
        ${CMAKE_CURRENT_LIST_DIR}/ParserValue.cpp
)
