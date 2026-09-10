find_package(GTest REQUIRED)

# 测试目录结构与 src/Base 的三个子目录一一对应，全部链接进同一个 TestBase 可执行文件
add_executable(TestBase
        BaseTestSupport.h

        Config/TestConfigValueType.cpp
        Config/TestConfigValue.cpp
        Config/TestConfigSchema.cpp
        Config/TestConfigManager.cpp

        Exception/TestException.cpp

        Log/TestLogLevel.cpp
        Log/TestLogColor.cpp
        Log/TestSourceLocation.cpp
        Log/TestLogEvent.cpp
        Log/TestLogMacros.cpp
        Log/TestDefaultFormatter.cpp
        Log/TestColorFormatter.cpp
        Log/TestLogSink.cpp
        Log/TestConsoleSink.cpp
        Log/TestFileSink.cpp
        Log/TestRollingFileSink.cpp
        Log/TestAsyncSink.cpp
        Log/TestLogger.cpp
        Log/TestLoggerRegistry.cpp
        Log/TestLoggerConfigLoader.cpp
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
