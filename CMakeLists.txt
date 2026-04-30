cmake_minimum_required(VERSION 3.20)

project(AsynGyanis
        VERSION 1.0.0
        DESCRIPTION "Industrial-grade C++20 async server framework based on epoll"
        LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(jemalloc REQUIRED)
find_package(Catch2 REQUIRED)
find_package(llhttp REQUIRED)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
add_subdirectory(samples)