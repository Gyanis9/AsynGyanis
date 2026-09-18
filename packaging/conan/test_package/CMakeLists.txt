cmake_minimum_required(VERSION 3.20)

project(AsynGyanisConsumerSmoke LANGUAGES CXX)

# 消费方视角：只需要 find_package(AsynGyanis)，不需要知道包内有几个目标、头文件在哪、
# 依赖怎么拼——这些都是包该负责的。链 AsynGyanis::Net 即由它拉齐 Core/Base/Platform 与
# 外部依赖（zlib/openssl/sqlite3/hiredis）
find_package(AsynGyanis REQUIRED)

add_executable(consumer_smoke main.cpp)
target_link_libraries(consumer_smoke PRIVATE AsynGyanis::Net)
target_compile_features(consumer_smoke PRIVATE cxx_std_23)
