# 基础模块按职责分三个子目录：Log（日志）、Config（配置）、Exception（异常）
add_library(Base
        Config/ConfigValueType.h
        Config/ConfigValueType.cpp
        Config/ConfigValue.h
        Config/ConfigValue.cpp
        Config/ConfigLoadResult.h
        Config/ConfigSchema.h
        Config/ConfigSchema.cpp
        Config/ConfigValidationResult.h
        Config/ConfigManager.h
        Config/ConfigManager.cpp

        Exception/Exception.h
        Exception/Exception.cpp
        Exception/SystemException.h
        Exception/SystemException.cpp
        Exception/NetworkException.h
        Exception/NetworkException.cpp
        Exception/ConfigException.h
        Exception/ConfigException.cpp
        Exception/ConfigFileException.h
        Exception/ConfigFileException.cpp
        Exception/ConfigParseException.h
        Exception/ConfigParseException.cpp
        Exception/ConfigKeyNotFoundException.h
        Exception/ConfigKeyNotFoundException.cpp
        Exception/ConfigTypeException.h
        Exception/ConfigTypeException.cpp
        Exception/ConfigValidationException.h
        Exception/ConfigValidationException.cpp

        Log/LogLevel.h
        Log/LogColor.h
        Log/SourceLocation.h
        Log/LogEvent.h
        Log/LogMacros.h
        Log/LogFormatter.h
        Log/DefaultFormatter.h
        Log/DefaultFormatter.cpp
        Log/ColorFormatter.h
        Log/ColorFormatter.cpp
        Log/LogSink.h
        Log/LogSink.cpp
        Log/ConsoleSink.h
        Log/ConsoleSink.cpp
        Log/FileSink.h
        Log/FileSink.cpp
        Log/RollingFileSink.h
        Log/RollingFileSink.cpp
        Log/AsyncSink.h
        Log/AsyncSink.cpp
        Log/Logger.h
        Log/Logger.cpp
        Log/LoggerRegistry.h
        Log/LoggerRegistry.cpp
        Log/LoggerConfigLoader.h
        Log/LoggerConfigLoader.cpp
)

target_link_libraries(Base PUBLIC
        Platform
        yaml-cpp::yaml-cpp
)

if (TARGET jemalloc::jemalloc)
    target_link_libraries(Base PUBLIC jemalloc::jemalloc)
endif ()

target_include_directories(Base PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(Base PUBLIC cxx_std_20)
