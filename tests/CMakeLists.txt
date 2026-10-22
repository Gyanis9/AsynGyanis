# 构建期枚举用例的时限，各目录的 gtest_discover_tests 共用这一份。
# 实测五个 ASan 测试二进制并发冷启动撞上 gtest 默认 5 秒超时（链接已完成，用例本身没失败）；
# 60 秒把这条判据从调度运气上摘下来——枚举列出即结束。
set(ASYN_GTEST_DISCOVERY_TIMEOUT 60)

add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)
add_subdirectory(Database)
add_subdirectory(Net)
# 测试期可执行体（外部探针的夹具端等），与上面五个 gtest 目录并列而不是混进去：它们不链接 GTest
add_subdirectory(Tools)

# 与 src/CMakeLists.txt 的模块开关保持同步：五个模块的实现与测试都已纳入构建
