find_package(Catch2 REQUIRED)

add_executable(TestBase
        TestConfigType.cpp
        TestException.cpp
        TestConfigValue.cpp
        TestConfigFileWatcher.cpp
        TestConfigManager.cpp
        TestLogCommon.cpp
        TestLogSink.cpp
        TestLogger.cpp
        TestLoggerConfig.cpp
)


target_link_libraries(TestBase PRIVATE
        Base
        Catch2::Catch2WithMain
)

target_include_directories(TestBase PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(TestBase PRIVATE cxx_std_20)

include(Catch)
catch_discover_tests(TestBase)