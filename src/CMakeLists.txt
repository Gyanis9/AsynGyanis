add_subdirectory(Platform)
add_subdirectory(Base)
add_subdirectory(Core)
add_subdirectory(Database)
add_subdirectory(Net)

# 五个模块全部纳入构建：Platform、Base、Core、Database、Net。
# 各模块的测试目录开关在 tests/CMakeLists.txt 中与本文件保持同步。
#
# 公开头文件均为 UTF-8（含中文注释），而 MSVC 默认按系统代码页解码源码：在 GBK 环境里
# 这会解码错位，把注释里的字节吞进下一行，报出「#endif 不匹配」这类假语法错误。
# 因此把 /utf-8 随接口一并传递给所有使用方（含安装后 find_package 的外部工程）。
foreach(asyn_module Platform Base Core Net Database)
    target_compile_options(${asyn_module} INTERFACE
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
endforeach()
