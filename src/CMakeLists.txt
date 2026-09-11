add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)

# 本轮重构推进 Platform、Base 与 Core；Net 与 Database 源码保留但不参与构建。
# 恢复方式：取消对应 add_subdirectory 注释，并同时启用 tests/CMakeLists.txt 中的测试目录。
# add_subdirectory(Net)
# add_subdirectory(Database)
