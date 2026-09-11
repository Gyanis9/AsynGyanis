# Base/Parser —— 手写 YAML 与 JSON 解析、JSON 序列化
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/ParserPosition.h
        ${CMAKE_CURRENT_LIST_DIR}/ParserError.h
        ${CMAKE_CURRENT_LIST_DIR}/ParserError.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ParserText.h
        ${CMAKE_CURRENT_LIST_DIR}/ParserText.cpp
        ${CMAKE_CURRENT_LIST_DIR}/JsonParser.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/JsonWriter.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonWriter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/YamlParser.h
        ${CMAKE_CURRENT_LIST_DIR}/YamlParser.cpp
)
