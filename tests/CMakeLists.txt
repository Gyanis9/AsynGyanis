add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)

# 与 src/CMakeLists.txt 的模块开关保持同步：Net 与 Database 恢复构建时再启用对应测试目录
# add_subdirectory(Net)
# add_subdirectory(Database)
