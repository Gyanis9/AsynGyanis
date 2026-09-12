add_executable(echo_server main.cpp)

target_link_libraries(echo_server PRIVATE
        Base
        Core
        Net
)

target_include_directories(echo_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(echo_server PRIVATE cxx_std_20)

# ASan 运行时拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
asyn_copy_sanitizer_runtime(echo_server)
