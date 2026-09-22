find_package(GTest REQUIRED)

# 测试目录结构与 src/Core 的四个子目录逐级对应，全部链接进同一个 TestCore 可执行文件
add_executable(TestCore
        CoreTestSupport.h
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/CommonTestSupport.h

        Exception/TestCoreException.cpp

        EventLoop/TestEpoll.cpp
        EventLoop/TestIoWatcher.cpp
        EventLoop/TestEventLoop.cpp
        EventLoop/TestIoContext.cpp
        EventLoop/TestConnectionDistributor.cpp
        EventLoop/TestTimer.cpp

        Coroutine/TestScheduler.cpp
        Coroutine/TestTask.cpp
        Coroutine/TestCoroutinePool.cpp
        Coroutine/TestCancelable.cpp
        Coroutine/TestThreadPool.cpp
        Coroutine/TestAsyncExecutor.cpp

        Process/TestWorkerSupervisor.cpp

        Socket/TestAsyncSocket.cpp
        Socket/TestAsyncUdpSocket.cpp
        Socket/TestAsyncResolver.cpp
        Socket/TestVectoredSendCursor.cpp
        Socket/TestInetAddress.cpp
        Socket/TestConnection.cpp
        Socket/TestConnectionManager.cpp

        Tls/TestTlsContext.cpp
        Tls/TestTlsSocket.cpp
)

target_link_libraries(TestCore PRIVATE
        Core
        Base
        GTest::gtest
        GTest::gtest_main
)

# TLS 测试使用的仓库内证书目录（tests/Core/fixtures）
target_compile_definitions(TestCore PRIVATE
        TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_include_directories(TestCore PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        # src 用于 "Core/..." 形式的生产头包含，本目录用于 "CoreTestSupport.h" 同级包含，
        # tests/TestSupport 用于复用 CommonTestSupport.h 的临时目录与等待夹具，
        # tests/Platform 用于复用 Platform 专有的描述符判据（下游模块复用上游测试头，方向与生产依赖一致）
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests/TestSupport
        ${CMAKE_SOURCE_DIR}/tests/Platform
)

target_compile_features(TestCore PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(TestCore)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestCore)
