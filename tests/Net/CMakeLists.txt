find_package(Catch2 REQUIRED)

add_executable(TestNet
        TestHttpRequest.cpp
        TestHttpResponse.cpp
        TestHttpParser.cpp
)

target_link_libraries(TestNet PRIVATE
        Net
        Catch2::Catch2WithMain
)

target_include_directories(TestNet PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(TestNet PRIVATE cxx_std_20)

include(Catch)
catch_discover_tests(TestNet)
