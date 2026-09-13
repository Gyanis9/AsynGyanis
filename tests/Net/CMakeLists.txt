find_package(GTest REQUIRED)

# 测试目录结构与 src/Net 逐级对应，全部链接进同一个 TestNet 可执行文件
add_executable(TestNet
        Http/HttpTestSupport.h

        Http/TestHttpParser.cpp
        Http/TestHttpParserChunked.cpp
        Http/TestHttpParserFuzzSmoke.cpp
        Http/TestHttpDate.cpp
        Http/TestHttpRequest.cpp
        Http/TestHttpResponse.cpp
        Http/TestHttpSession.cpp
        Http/TestHttpServer.cpp
        Http/TestHttpServerLimits.cpp
        Http/TestHttpObservability.cpp
        Http/TestRouter.cpp
        Http/TestMiddleware.cpp
        Http/TestFileSender.cpp

        Tcp/TestTcpStream.cpp
        Tcp/TestTcpAcceptor.cpp
        Tcp/TestTcpServer.cpp
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

target_compile_features(TestNet PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(TestNet)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(TestNet)
