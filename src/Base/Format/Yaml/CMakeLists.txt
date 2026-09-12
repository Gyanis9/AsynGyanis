# Base/Format/Yaml —— 严格 YAML 1.2 解析（DOM 前端 + 流式事件前端）
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/YamlParseOptions.h
        ${CMAKE_CURRENT_LIST_DIR}/YamlEvent.h
        ${CMAKE_CURRENT_LIST_DIR}/YamlReader.h
        ${CMAKE_CURRENT_LIST_DIR}/YamlReader.cpp
        ${CMAKE_CURRENT_LIST_DIR}/YamlParser.h
        ${CMAKE_CURRENT_LIST_DIR}/YamlParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/YamlWriteOptions.h
        ${CMAKE_CURRENT_LIST_DIR}/YamlWriter.h
        ${CMAKE_CURRENT_LIST_DIR}/YamlWriter.cpp
)
