add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)
add_subdirectory(Database)
add_subdirectory(Net)
# 测试期可执行体（外部探针的夹具端等），与上面五个 gtest 目录并列而不是混进去：它们不链接 GTest
add_subdirectory(Tools)

# 与 src/CMakeLists.txt 的模块开关保持同步：五个模块的实现与测试都已纳入构建
