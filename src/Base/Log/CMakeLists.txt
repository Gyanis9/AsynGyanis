# Base/Log —— 日志等级、事件、源码位置、日志器与注册表；
# 格式化器与输出目标各自再分子目录
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/LogLevel.h
        ${CMAKE_CURRENT_LIST_DIR}/LogColor.h
        ${CMAKE_CURRENT_LIST_DIR}/SourceLocation.h
        ${CMAKE_CURRENT_LIST_DIR}/LogEvent.h
        ${CMAKE_CURRENT_LIST_DIR}/LogMacros.h
        ${CMAKE_CURRENT_LIST_DIR}/LogThrottle.h
        ${CMAKE_CURRENT_LIST_DIR}/LogThrottle.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Logger.h
        ${CMAKE_CURRENT_LIST_DIR}/Logger.cpp
        ${CMAKE_CURRENT_LIST_DIR}/LoggerRegistry.h
        ${CMAKE_CURRENT_LIST_DIR}/LoggerRegistry.cpp
        ${CMAKE_CURRENT_LIST_DIR}/LoggerConfigLoader.h
        ${CMAKE_CURRENT_LIST_DIR}/LoggerConfigLoader.cpp
)

add_subdirectory(Formatters)
add_subdirectory(Sinks)
