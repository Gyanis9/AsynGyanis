find_package(GTest REQUIRED)

# 测试目录结构与 src/Base 逐级对应（含 Log/Sinks 与 Log/Formatters），
# 全部链接进同一个 TestBase 可执行文件
add_executable(TestBase
        BaseTestSupport.h

        Config/TestConfigValueType.cpp
        Config/TestConfigSchema.cpp
        Config/TestConfigManager.cpp

        Exception/TestException.cpp

        Log/TestLogLevel.cpp
        Log/TestLogColor.cpp
        Log/TestSourceLocation.cpp
        Log/TestLogEvent.cpp
        Log/TestLogMacros.cpp
        Log/TestLogger.cpp
        Log/TestLoggerRegistry.cpp
        Log/TestLoggerConfigLoader.cpp

        Log/Formatters/TestDefaultFormatter.cpp
        Log/Formatters/TestColorFormatter.cpp

        Log/Sinks/TestLogSink.cpp
        Log/Sinks/TestConsoleSink.cpp
        Log/Sinks/TestFileSink.cpp
        Log/Sinks/TestRollingFileSink.cpp
        Log/Sinks/TestAsyncSink.cpp

        Format/TestTextPositionAndError.cpp
        Format/Value/TestFormatValue.cpp
        Format/Value/TestValueAccessError.cpp
        Format/Json/TestJsonParser.cpp
        Format/Json/TestJsonWriter.cpp
        Format/Json/TestJsonPointer.cpp
        Format/Json/TestJsonPatch.cpp
        Format/Json/TestJsonStream.cpp
        Format/Yaml/TestYamlParser.cpp
        Format/Yaml/TestYamlScalars.cpp
        Format/Yaml/TestYamlAnchors.cpp
        Format/Yaml/TestYamlReader.cpp
        Format/Yaml/TestYamlWriter.cpp
)

target_link_libraries(TestBase PRIVATE
        Base
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(TestBase PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(TestBase PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(TestBase)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestBase)
