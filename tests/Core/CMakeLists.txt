find_package(GTest REQUIRED)

# 测试目录结构与 src/Core 的各子目录逐级对应，全部链接进同一个 TestCore 可执行文件
add_executable(TestCore
        CoreTestSupport.h
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/CommonTestSupport.h
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/MetricsTestSupport.h
        # 分配画像探针的实体：TestScheduler 的跨线程投递读数靠它，口径与 Base/Net 两侧一致
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/AllocationProbe.cpp
        # 交付形态守卫的实体：TlsContext 那批直连用例要靠它判「用例侧与库侧是否各持一份 OpenSSL」
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/SharedFormGuards.cpp

        Exception/TestCoreException.cpp

        EventLoop/TestEventPoller.cpp
        EventLoop/TestIoWatcher.cpp
        EventLoop/TestEventLoop.cpp
        EventLoop/TestEventLoopObservability.cpp
        EventLoop/TestIoContext.cpp
        EventLoop/TestConnectionDistributor.cpp
        EventLoop/TestTimer.cpp

        Crypto/TestDigest.cpp
        Crypto/TestRandom.cpp
        Crypto/TestHkdf.cpp

        Metrics/TestProcessMetricsRegistry.cpp

        Coroutine/TestScheduler.cpp
        Coroutine/TestSemaphore.cpp
        Coroutine/TestMutex.cpp
        Coroutine/TestTask.cpp
        Coroutine/TestLogContext.cpp
        Coroutine/TestCoroutinePool.cpp
        Coroutine/TestDeadlineGuard.cpp
        Coroutine/TestCancellationWatch.cpp
        Coroutine/TestCancelable.cpp
        Coroutine/TestCursorRange.cpp
        Coroutine/TestThreadPool.cpp
        Coroutine/TestAsyncExecutor.cpp

        Process/TestGracefulShutdown.cpp
        Process/TestReloadSignal.cpp
        Process/TestServiceWatchdog.cpp
        Process/TestRoundTicker.cpp
        Process/TestWorkerHeartbeat.cpp
        Process/TestUpgradeChannel.cpp
        Process/TestWorkerSupervisor.cpp
        Process/TestCrashScene.cpp

        Socket/TestAsyncSocket.cpp
        Socket/TestConnectionRace.cpp
        Socket/TestAsyncUdpSocket.cpp
        Socket/TestAsyncResolver.cpp
        Socket/TestVectoredSendCursor.cpp
        Socket/TestInetAddress.cpp
        Socket/TestConnection.cpp
        Socket/TestConnectionManager.cpp

        Tls/TestTlsContext.cpp
        Tls/TestTlsPolicy.cpp
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
        # 多进程编排用例要派生的 worker 侧夹具（tests/Tools/HandoffWorker.cpp）：用例只拿到路径，
        # 判定留在自己这一侧，避免「同一个进程里自己测自己」把跨进程那条路径测成同进程
        ASYN_HANDOFF_WORKER_TOOL="$<TARGET_FILE:handoff_worker>"
        # 看门狗汇聚用例要派生的心跳 worker 夹具（tests/Tools/HeartbeatWorker.cpp）：同上，跨进程
        # 那条路径（建管道 → fork 继承 → worker 起拍）只有真的另起一个进程才走得到
        ASYN_HEARTBEAT_WORKER_TOOL="$<TARGET_FILE:heartbeat_worker>"
)

# 夹具必须在 TestCore 之前建出来：上面那条定义按生成期展开，路径指向的产物不存在时用例会在
# 派生子进程那一步红，红成「编排坏了」而不是「没构建齐」
add_dependencies(TestCore handoff_worker heartbeat_worker)

target_include_directories(TestCore PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        # src 用于 "Core/..." 头包含，本目录用于 "CoreTestSupport.h" 同级包含；
        # tests/TestSupport 复用 CommonTestSupport.h 的临时目录与等待夹具，
        # tests/Platform 复用 Platform 的描述符判据（下游复用上游测试头，方向与生产依赖一致）
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests/TestSupport
        ${CMAKE_SOURCE_DIR}/tests/Platform
)

target_compile_features(TestCore PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(TestCore
        DISCOVERY_TIMEOUT ${ASYN_GTEST_DISCOVERY_TIMEOUT})

# ASan 运行时拷到本目标输出目录（见 cmake/AsynGyanisSanitizers.cmake）
asyn_copy_sanitizer_runtime(TestCore)
