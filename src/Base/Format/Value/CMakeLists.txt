# Base/Format/Value —— 跨格式共用的文档值模型与其错误类型
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/FormatValueType.h
        ${CMAKE_CURRENT_LIST_DIR}/FormatValueType.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ValueAccessError.h
        ${CMAKE_CURRENT_LIST_DIR}/ValueAccessError.cpp
        ${CMAKE_CURRENT_LIST_DIR}/FormatValue.h
        ${CMAKE_CURRENT_LIST_DIR}/FormatValue.cpp
)
