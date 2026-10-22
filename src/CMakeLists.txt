add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)
add_subdirectory(Database)
add_subdirectory(Net)

# 五个模块全部纳入构建，顺序 Platform、Base、Core、Database、Net；tests 的目录开关同步。
# 公开头文件均为 UTF-8，MSVC 默认按系统代码页解码会在 GBK 环境报「#endif 不匹配」的假语法错误，
# 因此把 /utf-8 随接口传给所有使用方（含安装后 find_package 的外部工程）。
foreach(asyn_module Platform Base Core Net Database)
    target_compile_options(${asyn_module} INTERFACE
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
endforeach()
