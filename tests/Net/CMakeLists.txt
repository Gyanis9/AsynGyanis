find_package(GTest REQUIRED)

# 测试目录结构与 src/Net 逐级对应，全部链接进同一个 TestNet 可执行文件
add_executable(TestNet
        Http/TestHttpParser.cpp
        Http/TestHttpRequest.cpp
        Http/TestHttpResponse.cpp
        Http/TestHttpSession.cpp
        Http/TestHttpServer.cpp
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

target_include_directories(TestNet PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(TestNet PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(TestNet)
