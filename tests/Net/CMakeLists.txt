find_package(GTest REQUIRED)

# 测试目录结构与 src/Net 逐级对应，全部链接进同一个 TestNet 可执行文件
add_executable(TestNet
        NetTestSupport.h
        Quic/QuicRfcVectors.h
        Http/HttpTestSupport.h
        Http2/Http2TestSupport.h
        # 协议解码器的属性化随机模糊：内核与用例（libFuzzer 入口另见文件末尾的可选目标）
        Fuzz/ProtocolFuzzKernel.h
        Fuzz/ProtocolFuzzKernel.cpp
        Fuzz/TestProtocolFuzz.cpp
        # 分配计数探针的实体（替换全局 operator new，每个可执行体只能有一份定义）
        ${CMAKE_SOURCE_DIR}/tests/TestSupport/AllocationProbe.cpp

        Http/TestHttpParser.cpp
        Http/TestHttpParserLimits.cpp
        Http/TestHttpParserChunked.cpp
        Http/TestHttpParserFuzzSmoke.cpp
        Http/TestHttpDate.cpp
        Http/TestHttpHeaderRules.cpp
        Http/TestHttpHeaderFieldStore.cpp
        Http/TestHotPathAllocations.cpp
        Http/TestHttpRequest.cpp
        Http/TestHttpRequestId.cpp
        Http/TestHttpResponse.cpp
        Http/TestHttpSession.cpp
        Http/TestHttpStreaming.cpp
        Http/TestHttpStreamingBody.cpp
        Http/TestHttpStreamBody.cpp
        Http/TestSseStream.cpp
        Http/TestHttpServer.cpp
        Http/TestHttpStaticFileTransfer.cpp
        Http/TestHttpClient.cpp
        Http/TestHttpResponseParser.cpp
        Http/TestHttpServerLimits.cpp
        Http/TestStaticFileMappingCache.cpp
        Http/TestGzip.cpp
        Http/TestCompressionCodecs.cpp
        Quic/TestQuicServer.cpp
        Quic/Codec/TestQuicVariableLengthInteger.cpp
        Quic/Codec/TestQuicPacketHeader.cpp
        Quic/Codec/TestQuicFrame.cpp
        Quic/Codec/TestQuicTransportParameters.cpp
        Quic/Crypto/TestQuicKeySchedule.cpp
        Quic/Crypto/TestQuicHeaderProtection.cpp
        Quic/Crypto/TestQuicPacketProtection.cpp
        Quic/Crypto/TestQuicTlsContext.cpp
        Quic/TestQuicPacketBuilder.cpp
        Quic/TestQuicConnection.cpp
        Quic/TestQuicConnectionCore.cpp
        Quic/Recovery/TestQuicRecovery.cpp
        Quic/Recovery/TestQuicCongestionControl.cpp
        Quic/TestQuicReassemblyBuffer.cpp
        Quic/TestQuicReceivedPacketNumbers.cpp
        Quic/Streams/TestQuicStreamLayer.cpp
        Quic/Streams/TestQuicStreamLayerHotPathAllocations.cpp
        Http3/TestHttp3Error.cpp
        Http3/TestHttp3Frame.cpp
        Http3/TestHttp3HeaderValidation.cpp
        Http3/TestHttp3Connection.cpp
        Http3/TestQpack.cpp
        Http3/TestHttp3Session.cpp
        WebSocket/TestPerMessageDeflate.cpp
        Http/TestCompressionMiddleware.cpp
        Http/TestHttpAcceptDispatch.cpp
        Http/TestHttpMemoryBudget.cpp
        Http/TestHttpServerConfig.cpp
        Http/TestHttpServerParserLimits.cpp
        Http/TestHttpObservability.cpp
        Http/TestHttpMetricsEndpoint.cpp
        Http/TestRouter.cpp
        Http/TestMiddleware.cpp
        Http/TestFileSender.cpp
        Http/TestHttpsServer.cpp

        Tcp/TestTcpStream.cpp
        Tcp/TestTcpClient.cpp
        Tcp/TestTcpAcceptor.cpp
        Tcp/TestSocketTuning.cpp
        Tcp/TestTcpServer.cpp
        Tcp/TestPerIpConnectionLimiter.cpp

        WebSocket/TestWebSocketHandshake.cpp
        WebSocket/TestWebSocketFrame.cpp
        WebSocket/TestWebSocketSession.cpp
        WebSocket/TestWebSocketUtf8.cpp

        Http2/TestHttp2Frame.cpp
        Http2/TestHpack.cpp
        Http2/TestHttp2Connection.cpp
        Http2/TestHttp2Session.cpp
        Http2/TestHttp2CleartextSession.cpp
        Http2/TestHttp2FuzzSmoke.cpp
)

# 压缩用例直接调用三种库的解码器做往返自检：目标由 src/Net 里 find_package 得到，
# 但 imported 目标只在创建它的目录及其子目录可见（tests 是兄弟目录），这里重新找一次即可
find_package(ZLIB REQUIRED)
find_package(zstd REQUIRED)
find_package(brotli REQUIRED)

# HTTP/3 用例要直接用 nghttp3 的客户端侧当对端（Net 对 nghttp3 是私有链接、头文件不外泄），因此这里也重新找一次
find_package(nghttp3 REQUIRED)

# QUIC 用例同样直接用 ngtcp2 搭一个独立的客户端当裁判：传输层自研之后 Net 已经不链接它，
# 这份依赖就只剩测试这一处（NGTCP2_STATICLIB 必须一起给，漏了会按 dllimport 声明去链接）
target_link_libraries(TestNet PRIVATE
        Net
        ngtcp2_static
        ngtcp2_crypto_ossl_static
        ZLIB::ZLIB
        zstd::libzstd
        brotli::brotli
        nghttp3::nghttp3
        GTest::gtest
        GTest::gtest_main
)

target_compile_definitions(TestNet PRIVATE NGTCP2_STATICLIB)

# src 用于 "Net/..." 形式的生产头包含；其余三条是同级包含的测试辅助头所在目录
# （NetTestSupport.h 在本目录、HttpTestSupport.h 在 Http/、Http2TestSupport.h 在 Http2/）
target_include_directories(TestNet PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Http
        ${CMAKE_CURRENT_SOURCE_DIR}/Http2
        ${CMAKE_CURRENT_SOURCE_DIR}/Quic
        ${CMAKE_SOURCE_DIR}/tests/Core
        ${CMAKE_SOURCE_DIR}/tests/TestSupport
)

# HTTPS 端到端用例使用的仓库内证书目录（与 tests/Core/fixtures 同一份自签证书）
target_compile_definitions(TestNet PRIVATE
        TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/Core/fixtures"
)

target_compile_features(TestNet PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(TestNet)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestNet)

# libFuzzer 入口：默认不构建。它要 clang 的 -fsanitize=fuzzer，而本机能拿到的 clang（llvm-mingw 22.1.7）
# 对 windows-gnu 目标根本不支持这个 sanitizer，常用容器里也没装——因此单独立一个开关，
# 由 CI 的 fuzz 作业（自己 apt 装 clang）显式打开。开关打开但不是 Clang 时当场拒绝，
# 不要静默产出一个「链接成功但没有模糊能力」的可执行体。
option(ASYN_BUILD_FUZZ_TARGETS "构建 Net 协议解码器的 libFuzzer 目标（需要 Clang）" OFF)
if(ASYN_BUILD_FUZZ_TARGETS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ASYN_BUILD_FUZZ_TARGETS 需要 Clang 工具链（-fsanitize=fuzzer 是 clang 提供的）；"
                            "当前编译器是 ${CMAKE_CXX_COMPILER_ID}。请改用 gcc/clang 之外的持续模糊方案，或关掉本开关")
    endif()

    add_executable(NetFuzzTargets Fuzz/FuzzTargets.cpp Fuzz/ProtocolFuzzKernel.cpp)
    target_include_directories(NetFuzzTargets PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(NetFuzzTargets PRIVATE Net)
    target_compile_features(NetFuzzTargets PRIVATE cxx_std_23)
    # -fsanitize=fuzzer-no-link 用于对象编译（否则每个 TU 都会塞进一个 main），链接时才用完整版本
    target_compile_options(NetFuzzTargets PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    target_link_options(NetFuzzTargets PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
