# Base/Format/Json —— JSON 解析与序列化
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/JsonParseOptions.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonWriteOptions.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonParser.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/JsonWriter.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonWriter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/JsonPointer.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonPointer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/JsonPatch.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonPatch.cpp
        ${CMAKE_CURRENT_LIST_DIR}/JsonMergePatch.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonMergePatch.cpp
        ${CMAKE_CURRENT_LIST_DIR}/JsonReader.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonReader.cpp
        ${CMAKE_CURRENT_LIST_DIR}/JsonStreamWriter.h
        ${CMAKE_CURRENT_LIST_DIR}/JsonStreamWriter.cpp
)
