find_package(Catch2 REQUIRED)

add_executable(TestCore
        TestCore.cpp
        TestEpoll.cpp
        TestInetAddress.cpp
        TestAsyncSocket.cpp
        TestTask.cpp
        TestScheduler.cpp
        TestTimer.cpp
        TestBufferPool.cpp
        TestCoroutinePool.cpp
        TestCancelable.cpp
        TestConnection.cpp
        TestConnectionManager.cpp
        TestEventLoop.cpp
        TestThreadPool.cpp
        TestIoContext.cpp
        TestTlsContext.cpp
        TestTlsSocket.cpp
)

target_link_libraries(TestCore PRIVATE
        Core
        Base
        Catch2::Catch2WithMain
)

target_include_directories(TestCore PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(TestCore PRIVATE cxx_std_20)

include(Catch)
catch_discover_tests(TestCore)
