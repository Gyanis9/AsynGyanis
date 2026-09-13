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
        Http/TestSseStream.cpp
        Http/TestHttpServer.cpp
        Http/TestHttpServerLimits.cpp
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

target_link_libraries(TestNet PRIVATE
        Net
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
