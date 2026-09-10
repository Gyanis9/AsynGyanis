find_package(GTest REQUIRED)

# 测试目录结构与 src/Platform 的三个子目录一一对应
add_executable(TestPlatform
        PlatformTestSupport.h

        IO/TestFileDescriptor.cpp
        IO/TestEventNotifier.cpp
        IO/TestTimerFileDescriptor.cpp
        IO/TestConsole.cpp
        IO/TestSocket.cpp

        FileSystem/TestFileSystem.cpp
        FileSystem/TestAtomicFileWriter.cpp
        FileSystem/TestFileWatcher.cpp

        System/TestPlatformError.cpp
        System/TestPlatformTime.cpp
        System/TestProcessInfo.cpp
        System/TestTextEncoding.cpp
)

target_link_libraries(TestPlatform PRIVATE
        Platform
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(TestPlatform PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(TestPlatform PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(TestPlatform)
