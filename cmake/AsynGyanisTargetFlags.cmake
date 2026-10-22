# 本仓目标的告警与加固旗标；只作用于显式列出的目标。
include_guard(GLOBAL)

function(asyn_enable_strict_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /WX)
    else()
        # 指定初始化器只填当前分支用得上的字段是本仓写法；-Wcomment 误报路由模式里的 /*。
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Werror
                -Wno-missing-field-initializers -Wno-comment)
    endif()
endfunction()

function(asyn_apply_hardening target_name)
    if(NOT asynHardeningActive)
        return()
    endif()

    if(MSVC)
        # CFG 的链接策略只给本仓可执行体，静态库消费者自行决定。
        target_compile_options(${target_name} PRIVATE $<$<CONFIG:Release>:/guard:cf>)
        target_link_options(${target_name} PRIVATE $<$<CONFIG:Release>:/guard:cf>)
    else()
        # -U 在前：部分发行版预定义了 _FORTIFY_SOURCE，直接 -D 会撞 -Werror。
        # _FORTIFY_SOURCE 只在 Release 有意义：-O0 下 glibc 会退到未检查版本。
        target_compile_options(${target_name} PRIVATE
                -fstack-protector-strong
                $<$<CONFIG:Release>:-U_FORTIFY_SOURCE>
                $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=2>)
        target_link_options(${target_name} PRIVATE -Wl,-z,relro -Wl,-z,now)
    endif()
endfunction()

# 目标清单只在测试/示例开关打开时包含对应目标；TARGET 守卫兜住未定义的目标。
function(asyn_apply_project_target_flags)
    set(targets Platform Base Core Net Database)
    if(ASYN_BUILD_TESTS)
        list(APPEND targets TestPlatform TestBase TestCore TestNet TestDatabase microbench quic_probe_server)
    endif()
    if(ASYN_BUILD_SAMPLES)
        list(APPEND targets ReferenceServer)
    endif()
    foreach(target_name IN LISTS targets)
        if(TARGET ${target_name})
            asyn_enable_strict_warnings(${target_name})
            asyn_apply_hardening(${target_name})
        endif()
    endforeach()
endfunction()
