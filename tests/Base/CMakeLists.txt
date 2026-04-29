find_package(Catch2 REQUIRED)
include(Catch)

set(TEST_TARGETS
    TestConfigType
    TestException
    TestConfigValue
    TestConfigFileWatcher
    TestConfigManager
    TestLogCommon
    TestLogSink
    TestLogger
    TestLoggerConfig
)

foreach(target ${TEST_TARGETS})
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} PRIVATE Base Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(${target} PRIVATE cxx_std_20)
    catch_discover_tests(${target})
endforeach()
