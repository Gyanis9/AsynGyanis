# 热路径微基准：独立可执行文件，与 samples 同样随常规构建一起产出。
# 只链接库本身，不需要 gtest——它量的是库的热路径，不是库的行为。
add_executable(microbench main.cpp)

target_link_libraries(microbench PRIVATE
        Base
        Core
        Net
)

target_include_directories(microbench PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(microbench PRIVATE cxx_std_23)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(microbench)
