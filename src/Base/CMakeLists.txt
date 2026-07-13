add_library(Base
        ConfigType.cpp
        ConfigType.h
        ConfigValue.cpp
        ConfigValue.h
        ConfigFileWatcher.h
        ConfigManager.h
        ConfigManager.cpp
        ConfigFileWatcher.cpp
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

if(WIN32)
    target_sources(Base PRIVATE
            Win32FileWatcher.h
            Win32FileWatcher.cpp
    )
endif()

target_link_libraries(Base PUBLIC
        yaml-cpp::yaml-cpp
        Platform
)

if(TARGET jemalloc::jemalloc)
    target_link_libraries(Base PUBLIC jemalloc::jemalloc)
endif()

target_include_directories(Base PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)
