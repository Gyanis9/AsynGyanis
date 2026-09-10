add_subdirectory(Platform)
add_subdirectory(Base)

# 本轮重构只推进 Platform 与 Base，以下模块源码保留但不参与构建。
# 恢复方式：取消对应 add_subdirectory 注释，并同时启用 tests/CMakeLists.txt 中的测试目录。
# add_subdirectory(Core)
# add_subdirectory(Net)
# add_subdirectory(Database)
