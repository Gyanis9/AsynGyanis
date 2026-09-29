# 共享库（.so / .dll）形态的地基：库种类开关、符号导出宏的平台差异、可见性预设。
#
# 为什么不用 CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS：它靠扫目标文件猜要导出的符号，数据成员与静态成员
# 猜不出来，漏掉的符号在链接期才报，报出来的是「某个类的静态成员找不到」这种和真实原因（本框架
# 从来没打算导出符号）脱节的错。导出面必须由写代码的人显式标，链接器只负责在我们漏标时把话说清楚。

include_guard(GLOBAL)

# 库种类只认 CMake 的标准开关 BUILD_SHARED_LIBS（默认关）：Conan 的 shared 选项、vcpkg 的
# triplet 都直接映射到它，不必再造一个本仓专属的开关。这里把它折成一个可读的变量给各模块用，
# 是因为 add_library 的 KIND 位置不接受布尔值。
if(BUILD_SHARED_LIBS)
    set(ASYN_LIBRARY_KIND SHARED)
else()
    set(ASYN_LIBRARY_KIND STATIC)
endif()

# 每个模块一个导出宏前缀（ASYN_<模块>_API）。五个模块各是一份独立的库，
# 谁在构建自己就把「导出」那一半打开，别人用它就是「导入」那一半。
function(asyn_configure_module_library target)
    string(TOUPPER "${target}" asyn_target_upper)

    set_target_properties(${target} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            CXX_EXTENSIONS OFF)

    if(ASYN_LIBRARY_KIND STREQUAL "SHARED")
        # 默认隐藏：没标导出宏的符号出不去。这样导出面就是「代码里写了 ASYN_x_API 的那些」，
        # 而不是「所有公共头里的东西」——后者会让下一次重构不知不觉换掉线上符号
        set_target_properties(${target} PROPERTIES
                CXX_VISIBILITY_PRESET hidden
                VISIBILITY_INLINES_HIDDEN ON)
        # 构库自身看到 dllexport，消费者看到 dllimport：靠这两条 PUBLIC/PRIVATE 定义区分。
        # 反过来「什么都不定义」就是静态——fuzz 脚本这类绕过 CMake 直接编源文件的场合必须落在安全的一边
        target_compile_definitions(${target} PUBLIC ASYN_${asyn_target_upper}_SHARED_LIB)
        target_compile_definitions(${target} PRIVATE ASYN_${asyn_target_upper}_BUILDING_SHARED_LIB)
        # 显式写在而不是靠默认值：这份脚手架的存在理由就是「导出面由人标」，让它自动猜一次就白立了
        set_target_properties(${target} PROPERTIES
                WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    endif()
endfunction()
