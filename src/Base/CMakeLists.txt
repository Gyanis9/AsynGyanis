add_library(Base
        ConfigType.cpp
        ConfigType.h
        ConfigValue.cpp
        ConfigValue.h
        ConfigFileWatcher.h
        ConfigManager.h
        ConfigManager.cpp
        ConfigFileWatcher.cpp
        Expection.cpp
        Expection.h
        Exception.cpp
        Exception.h
        Config.h
        LogCommon.h
        LogSink.cpp
        LogSink.h
        Logger.cpp
        Logger.h
        LoggerConfig.cpp
        LoggerConfig.h
)

target_link_libraries(Base PUBLIC
        yaml-cpp::yaml-cpp
)

target_include_directories(Base PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)
