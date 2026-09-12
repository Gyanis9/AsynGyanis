add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)
add_subdirectory(Database)
add_subdirectory(Net)

# 五个模块全部纳入构建：Platform、Base、Core、Database、Net。
# 各模块的测试目录开关在 tests/CMakeLists.txt 中与本文件保持同步。
