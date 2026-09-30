# 构建期枚举用例的时限（各目录的 gtest_discover_tests 共用这一份，别再各写一个字面量）。
# 实测一次 Windows 作业里五个测试二进制**同时**卡在默认那 5 秒上，报的都是
# `Result: Process terminated due to timeout / Timeout: '5'`——链接已完成、用例也没失败，
# 慢在 ASan 版可执行体的冷启动（同一次构建里五份枚举由 ninja 并发发起，还要抢同一块盘）。
# 给到 60 秒不是「等慢一点」，是把这条判据从调度运气上摘下来：枚举本身列出即结束。
set(ASYN_GTEST_DISCOVERY_TIMEOUT 60)

add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)
add_subdirectory(Database)
add_subdirectory(Net)
# 测试期可执行体（外部探针的夹具端等），与上面五个 gtest 目录并列而不是混进去：它们不链接 GTest
add_subdirectory(Tools)

# 与 src/CMakeLists.txt 的模块开关保持同步：五个模块的实现与测试都已纳入构建
