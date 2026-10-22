# sanitizer、覆盖率旗标与 MSVC ASan 运行时拷贝。
include_guard(GLOBAL)

if(ENABLE_SANITIZERS)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
        # gtest 未插桩，容器注解的 ABI 标记会对不上（LNK2038），只关注解。
        add_compile_definitions(_DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION _DISABLE_OPTIONAL_ANNOTATION)
        message(STATUS "Sanitizers: 已启用 AddressSanitizer（MSVC 无 UBSan / TSan；"
                       "因 gtest 未插桩而关闭容器注解，见本文件注释）")
    else()
        # -fno-omit-frame-pointer 让栈回溯完整。
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
        message(STATUS "Sanitizers: 已启用 AddressSanitizer + UndefinedBehaviorSanitizer")
    endif()
endif()

if(SANITIZE_THREADS)
    if(MSVC)
        message(FATAL_ERROR "SANITIZE_THREADS=ON 在 MSVC 上不可用：MSVC 不提供 ThreadSanitizer，请在 Linux（GCC/Clang）侧配置该构建")
    endif()
    if(ENABLE_SANITIZERS)
        message(FATAL_ERROR "SANITIZE_THREADS 与 ENABLE_SANITIZERS 互斥：ASan 与 TSan 不能同时插桩，请只开一个")
    endif()
    # -g 让竞态报告能指回源码行。
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
    message(STATUS "Sanitizers: 已启用 ThreadSanitizer（线程竞态检测；与 ASan 互斥）")
endif()

if(ASYN_ENABLE_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "ASYN_ENABLE_COVERAGE=ON 在 MSVC 上不可用：请在 Linux（GCC/Clang）侧配置该构建")
    endif()
    # -O0 让行归属与源码一致。
    add_compile_options(--coverage -O0)
    add_link_options(--coverage)
    message(STATUS "Coverage: 已用 gcov 插桩（跑完 ctest 与 samples/ 后用 gcovr 出报告）")
endif()

# 用法：在可执行目标定义之后调用 asyn_copy_sanitizer_runtime(<目标名>)。
function(asyn_copy_sanitizer_runtime target_name)
    if(NOT ENABLE_SANITIZERS OR NOT MSVC)
        return()
    endif()

    get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)

    # 两份都拷：Debug 产物实测并不只依赖 _dbg_ 那一份。
    foreach(runtime_name clang_rt.asan_dbg_dynamic-x86_64.dll clang_rt.asan_dynamic-x86_64.dll)
        add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${compiler_directory}/${runtime_name}"
                        "$<TARGET_FILE_DIR:${target_name}>"
                COMMENT "拷贝 AddressSanitizer 运行时（${runtime_name}）到 ${target_name} 输出目录")
    endforeach()
endfunction()
