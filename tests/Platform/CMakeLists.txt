find_package(GTest REQUIRED)

# 测试目录结构与 src/Platform 的三个子目录一一对应
add_executable(TestPlatform
        PlatformTestSupport.h
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/CommonTestSupport.h
        # 分配计数探针的实体（替换全局 operator new，每个可执行体只能有一份定义）
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/AllocationProbe.cpp

        TestHotPathAllocations.cpp

        IO/TestFileDescriptor.cpp
        IO/TestEventNotifier.cpp
        IO/TestTimerFileDescriptor.cpp
        IO/TestConsole.cpp
        IO/TestSocket.cpp
        IO/TestDatagramSocket.cpp
        IO/TestSocketZeroCopySend.cpp
        IO/TestMemoryMappedFile.cpp
        IO/TestFileContents.cpp

        FileSystem/TestFileSystem.cpp
        FileSystem/TestFileBasicInfo.cpp
        FileSystem/TestAtomicFileWriter.cpp
        FileSystem/TestFileWatcher.cpp

        System/TestCpuAffinity.cpp
        System/TestPlatformError.cpp
        System/TestPlatformTime.cpp
        System/TestProcess.cpp
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
        # 共享测试夹具（临时目录与等待）的实体所在目录
        ${CMAKE_SOURCE_DIR}/tests/TestSupport
)

target_compile_features(TestPlatform PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(TestPlatform)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestPlatform)
