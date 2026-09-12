# Base/Config —— 配置加载与热更新（值模型见 Base/Format/Value）
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/ConfigValueType.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigValueType.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConfigValue.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigKeyValueMap.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigLoadResult.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigSchema.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigSchema.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConfigValidationResult.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigManager.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigManager.cpp
)
