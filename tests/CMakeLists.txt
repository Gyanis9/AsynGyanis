add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)
add_subdirectory(Database)

# 与 src/CMakeLists.txt 的模块开关保持同步：Net 完成重构后再启用其测试目录
# add_subdirectory(Net)
