cmake_minimum_required(VERSION 3.20)

project(AsynGyanisVcpkgConsumerSmoke LANGUAGES CXX)

# 消费方视角：只需要 find_package(AsynGyanis)，不需要知道包内有几个目标、头文件在哪、
# 依赖怎么拼。用 vcpkg 工具链配置本工程即可（-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake），
# 装配好的包会被 find_package 直接找到。
# 链 AsynGyanis::Net 就应当自动带出 Core/Base/Platform 与外部依赖（zlib/openssl/sqlite3/hiredis）
find_package(AsynGyanis REQUIRED COMPONENTS Net)

add_executable(consumer_smoke main.cpp)
target_link_libraries(consumer_smoke PRIVATE AsynGyanis::Net)
target_compile_features(consumer_smoke PRIVATE cxx_std_23)

enable_testing()
add_test(NAME consumer_smoke_runs COMMAND consumer_smoke)
set_tests_properties(consumer_smoke_runs PROPERTIES PASS_REGULAR_EXPRESSION "AsynGyanis::Net 可用")
