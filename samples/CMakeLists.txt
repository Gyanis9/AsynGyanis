# 示例程序：每个子系统一个可独立运行的程序，各自自检并以退出码报告结果
#
# echo_server 是「部署形态」的服务器示例（多协议、多进程、配置文件），保留用于压测与上手；
# 其余按模块拆开的示例承担「把每一块单独跑通」的职责，脚本 scripts/run_samples.py 会把它们
# 全部跑一遍并按退出码判定系统完整性。

# 一个示例一个目标：源码放同名子目录，统一挂 common/ 作为头文件搜索路径
function(asyn_add_sample sample_name)
        add_executable(${sample_name} ${ARGN})
        target_link_libraries(${sample_name} PRIVATE Base Core Net)
        target_include_directories(${sample_name} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(${sample_name} PRIVATE cxx_std_23)
        # ASan 运行库拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
        asyn_copy_sanitizer_runtime(${sample_name})
endfunction()

# 部署形态的 HTTP/HTTPS/HTTP2/HTTP3 服务器
asyn_add_sample(echo_server main.cpp)

# Base：日志子系统（Sink、格式化器、注册表、配置驱动装配、异常带栈）
asyn_add_sample(base_log base_log/main.cpp)

# Base：配置子系统（多文件加载、点分路径取值、模式校验、热重载）
asyn_add_sample(base_config base_config/main.cpp)
