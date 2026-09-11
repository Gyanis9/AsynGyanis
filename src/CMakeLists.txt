add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)
add_subdirectory(Database)

# 本轮重构已推进 Platform、Base、Core 与 Database；Net 源码保留但暂不参与构建。
# 恢复方式：取消对应 add_subdirectory 注释，并同时启用 tests/CMakeLists.txt 中的测试目录。
# add_subdirectory(Net)
