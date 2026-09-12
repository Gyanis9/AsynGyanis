# Base/Format —— 文本格式层：值模型、各格式的解析/输出/流式接口，每种格式一个子目录
# 新增格式（如 XML）时只需再加一个子目录并在此登记
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/TextPosition.h
        ${CMAKE_CURRENT_LIST_DIR}/FormatError.h
        ${CMAKE_CURRENT_LIST_DIR}/FormatError.cpp
        ${CMAKE_CURRENT_LIST_DIR}/FormatErrorKind.h
        ${CMAKE_CURRENT_LIST_DIR}/TextEscapes.h
        ${CMAKE_CURRENT_LIST_DIR}/TextEscapes.cpp
)

add_subdirectory(Value)
add_subdirectory(Json)
add_subdirectory(Yaml)
