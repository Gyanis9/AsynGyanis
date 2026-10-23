# 构建开关与互斥校验：其它模块只读这里的选项，不重复定义。
include_guard(GLOBAL)

# sanitizer 与 mimalloc 互斥：mimalloc 会遮蔽 ASan/TSan 的分配拦截。
option(ENABLE_SANITIZERS "启用 AddressSanitizer（GCC/Clang 上并含 UBSan）" OFF)
option(SANITIZE_THREADS "启用 ThreadSanitizer（仅 GCC/Clang；与 ENABLE_SANITIZERS 互斥）" OFF)
option(ASYN_ENABLE_COVERAGE "用 gcov 插桩以生成行/分支覆盖率报告（GCC/Clang；配 gcovr 或 lcov 消费）" OFF)
option(ASYN_WITH_MIMALLOC "链接 mimalloc 作为全局分配器（生产构建默认开启；与 sanitizer 互斥，sanitizer 档显式关闭）" ON)
option(ASYN_WITH_IO_URING "Linux 事件后端改用 io_uring（需要内核 5.6+；Windows 无效）" OFF)
option(ASYN_ENABLE_HARDENING "给本仓产出的目标加上加固位（栈保护、_FORTIFY_SOURCE、FULL RELRO、MSVC 的 CFG）" ON)
option(ASYN_BUILD_TESTS "构建测试与热路径微基准" ON)
option(ASYN_BUILD_SAMPLES "构建示例程序" ON)
option(ASYN_ENABLE_INSTALL "生成安装规则与 CMake 包配置，供外部工程 find_package 消费" ON)

# 认 CMake 标准开关：Conan 与 vcpkg 都映射到 BUILD_SHARED_LIBS。
option(BUILD_SHARED_LIBS "把五个模块产出为共享库（.so / .dll）而不是静态库" OFF)

if(ASYN_WITH_MIMALLOC AND (ENABLE_SANITIZERS OR SANITIZE_THREADS))
    message(FATAL_ERROR "ASYN_WITH_MIMALLOC 与 sanitizer 互斥：mimalloc 会遮蔽分配拦截，请只开一个"
                        "（例如加 -DASYN_WITH_MIMALLOC=OFF，或关掉 ENABLE_SANITIZERS / SANITIZE_THREADS）")
endif()

if(ASYN_WITH_IO_URING AND WIN32)
    message(FATAL_ERROR "ASYN_WITH_IO_URING 只对 Linux 有效：Windows 侧的事件后端是完成端口（Iocp）")
endif()

if(ASYN_ENABLE_HARDENING AND (ENABLE_SANITIZERS OR SANITIZE_THREADS))
    message(STATUS "AsynGyanis: 加固随 sanitizer 关闭（两份检查叠在一起会让一次 trap 归因不清）")
endif()

# 生效判定只算一处；施加加固与查询加固态的地方都读它。
if(ASYN_ENABLE_HARDENING AND NOT ENABLE_SANITIZERS AND NOT SANITIZE_THREADS)
    set(asynHardeningActive TRUE)
else()
    set(asynHardeningActive FALSE)
endif()
