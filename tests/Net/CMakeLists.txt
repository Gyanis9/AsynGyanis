find_package(GTest REQUIRED)

# 测试目录结构与 src/Net 逐级对应，全部链接进同一个 TestNet 可执行文件
add_executable(TestNet
        Http/HttpTestSupport.h

        Http/TestHttpParser.cpp
        Http/TestHttpParserLimits.cpp
        Http/TestHttpParserChunked.cpp
        Http/TestHttpParserFuzzSmoke.cpp
        Http/TestHttpDate.cpp
        Http/TestHttpRequest.cpp
        Http/TestHttpResponse.cpp
        Http/TestHttpSession.cpp
        Http/TestHttpStreaming.cpp
        Http/TestHttpStreamingBody.cpp
        Http/TestSseStream.cpp
        Http/TestHttpServer.cpp
        Http/TestHttpStaticFileTransfer.cpp
        Http/TestHttpServerLimits.cpp
        Http/TestGzip.cpp
        Http/TestCompressionCodecs.cpp
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

target_link_libraries(TestNet PRIVATE
        Net
        ZLIB::ZLIB
        zstd::libzstd
        brotli::brotli
        GTest::gtest
        GTest::gtest_main
)

# src 用于 "Net/Http/..." 形式的生产头包含，本目录下的 Http 用于 "HttpTestSupport.h" 形式
target_include_directories(TestNet PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/Http
)

# HTTPS 端到端用例使用的仓库内证书目录（与 tests/Core/fixtures 同一份自签证书）
target_compile_definitions(TestNet PRIVATE
        TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/Core/fixtures"
)

target_compile_features(TestNet PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(TestNet)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestNet)
