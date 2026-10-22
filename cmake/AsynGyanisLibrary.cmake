# 共享库地基：库种类、导出宏与可见性预设。
# 不用 CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS：它猜不出数据成员与静态成员，漏标到链接期才报。
include_guard(GLOBAL)

# 只认 CMake 标准开关 BUILD_SHARED_LIBS；Conan 与 vcpkg 都映射到它。
if(BUILD_SHARED_LIBS)
    set(ASYN_LIBRARY_KIND SHARED)
else()
    set(ASYN_LIBRARY_KIND STATIC)
endif()

# 每个模块一个导出宏前缀 ASYN_<模块>_API；构建自己时导出，消费时导入。
function(asyn_configure_module_library target)
    string(TOUPPER "${target}" asyn_target_upper)

    set_target_properties(${target} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            CXX_EXTENSIONS OFF)

    if(ASYN_LIBRARY_KIND STREQUAL "SHARED")
        # 默认隐藏未标导出的符号，导出面就是代码里写了 ASYN_x_API 的那些。
        set_target_properties(${target} PROPERTIES
                CXX_VISIBILITY_PRESET hidden
                VISIBILITY_INLINES_HIDDEN ON)
        # 构库自身看到 dllexport，消费者看到 dllimport；静态形态两边都不定义。
        target_compile_definitions(${target} PUBLIC ASYN_${asyn_target_upper}_SHARED_LIB)
        target_compile_definitions(${target} PRIVATE ASYN_${asyn_target_upper}_BUILDING_SHARED_LIB)
        # 导出面由人标，不靠扫描目标文件猜。
        set_target_properties(${target} PROPERTIES
                WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    endif()
endfunction()
