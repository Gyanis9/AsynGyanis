# Base/Parser —— 手写文本解析层：跨格式基础设施在本层，每种格式一个子目录
# 新增格式（如 XML）时只需再加一个子目录并在此登记
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/ParserPosition.h
        ${CMAKE_CURRENT_LIST_DIR}/ParserError.h
        ${CMAKE_CURRENT_LIST_DIR}/ParserError.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ParserText.h
        ${CMAKE_CURRENT_LIST_DIR}/ParserText.cpp
)

add_subdirectory(Value)
add_subdirectory(Json)
add_subdirectory(Yaml)
