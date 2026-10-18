cmake_minimum_required(VERSION 3.20)

project(AsynGyanisConsumerSmoke LANGUAGES CXX)

# 消费方视角：只需要 find_package(AsynGyanis)，不需要知道包内有几个目标、头文件在哪、
# 依赖怎么拼——这些都是包该负责的。链 AsynGyanis::Net 即由它拉齐 Core/Base/Platform 与
# 外部依赖（zlib/openssl/sqlite3/hiredis）
find_package(AsynGyanis REQUIRED)

add_executable(consumer_smoke main.cpp)
# 两条都要链：main.cpp 里那组「模块 PUBLIC 宏 vs 能力清单」的对照读的是各模块自己发出去的定义，
# 而 DATABASE_HAS_* 挂在 Database 上——只链 Net 时这些宏根本进不了使用方的预处理环境，
# 对照会把「清单说有、这边没链接」误判成分叉（Linux 包内带 hiredis 时当场以 #error 变红）
target_link_libraries(consumer_smoke PRIVATE AsynGyanis::Net AsynGyanis::Database)
target_compile_features(consumer_smoke PRIVATE cxx_std_23)
