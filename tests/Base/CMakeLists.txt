find_package(GTest REQUIRED)

# 测试目录结构与 src/Base 逐级对应（含 Log/Sinks 与 Log/Formatters），
# 全部链接进同一个 TestBase 可执行文件
add_executable(TestBase
        BaseTestSupport.h
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/CommonTestSupport.h
        # 分配计数探针的实体（替换全局 operator new，每个可执行体只能有一份定义）
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/AllocationProbe.cpp

        Config/TestConfigValue.cpp
        Config/TestConfigValueType.cpp
        Config/TestConfigSchema.cpp
        Config/TestConfigManager.cpp
        Config/TestReloadRoundGate.cpp
        Config/TestConfigHotPathAllocations.cpp

        Exception/TestException.cpp

        Log/TestLogLevel.cpp
        Log/TestLogColor.cpp
        Log/TestSourceLocation.cpp
        Log/TestLogEvent.cpp
        Log/TestLogMacros.cpp
        Log/TestLogger.cpp
        Log/TestLoggerRegistry.cpp
        Log/TestLoggerConfigLoader.cpp
        Log/TestLogHotPathAllocations.cpp

        Log/Formatters/TestDefaultFormatter.cpp
        Log/Formatters/TestJsonFormatter.cpp
        Log/Formatters/TestColorFormatter.cpp
        Log/Formatters/TestTimestampText.cpp

        Log/Sinks/TestLogSink.cpp
        Log/Sinks/TestConsoleSink.cpp
        Log/Sinks/TestFileSink.cpp
        Log/Sinks/TestRollingFileSink.cpp
        Log/Sinks/TestRollingPeriod.cpp
        Log/Sinks/TestAsyncSink.cpp
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
        # 共享测试夹具（临时目录与等待）的实体所在目录
        ${CMAKE_SOURCE_DIR}/tests/TestSupport
)

target_compile_features(TestBase PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(TestBase)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestBase)
