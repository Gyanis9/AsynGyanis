find_package(GTest REQUIRED)

# 测试目录结构与 src/Net 逐级对应，全部链接进同一个 TestNet 可执行文件
add_executable(TestNet
        NetTestSupport.h
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
        Http/TestHttpStreamBody.cpp
        Http/TestSseStream.cpp
        Http/TestHttpServer.cpp
        Http/TestHttpStaticFileTransfer.cpp
        Http/TestHttpClient.cpp
        Http/TestHttpResponseParser.cpp
        Http/TestHttpServerLimits.cpp
        Http/TestGzip.cpp
        Http/TestCompressionCodecs.cpp
        Quic/TestQuicServer.cpp
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

target_link_libraries(TestNet PRIVATE
        Net
        ZLIB::ZLIB
        zstd::libzstd
        brotli::brotli
        nghttp3::nghttp3
        GTest::gtest
        GTest::gtest_main
)

# src 用于 "Net/..." 形式的生产头包含，Http 目录与 tests/Core 用于同级包含的测试辅助头
# （NetTestSupport.h 在本目录、HttpTestSupport.h 在 Http/、CoreTestSupport.h 在 tests/Core）
target_include_directories(TestNet PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Http
        ${CMAKE_SOURCE_DIR}/tests/Core
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
