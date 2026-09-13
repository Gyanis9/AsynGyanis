cmake_minimum_required(VERSION 3.20)

project(AsynGyanis
        VERSION 1.0.0
        DESCRIPTION "Industrial-grade C++20 async server framework"
        LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 事件循环只有两个后端：Linux 的 epoll（src/Core/EventLoop/Epoll.cpp）与 Windows 的 vendored
# wepoll（src/Core/EventLoop/wepoll）。其它系统在 Core 这一层就编译不过，而报出来的会是几百行
# 模板错误——与其让人去猜，不如在这里一句话说清「缺的是 kqueue 后端」以及补它的落点。
# README 的「环境要求」也写了这一点，两处一致
if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
            "AsynGyanis 目前只支持 Linux 与 Windows：事件循环的实现是 epoll 与 vendored wepoll，"
            "没有可用的 kqueue 后端（当前系统：${CMAKE_SYSTEM_NAME}）。要支持该系统，"
            "需要新增一个与 Core/EventLoop/Epoll.h 同接口的事件通知实现，并在 Core 的 CMakeLists 里按平台选用。")
endif()

# 各模块用 CMAKE_INSTALL_INCLUDEDIR 描述安装后的头文件根目录，故在子目录之前引入
include(GNUInstallDirs)

# MSVC: 强制 UTF-8 源码编码，解决中文注释和字符串在 GBK 环境下编译失败的问题
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ============================================================================
# 内存与未定义行为检测（Debug preset 里的 ENABLE_SANITIZERS）
# ============================================================================
# 此前 ENABLE_SANITIZERS 只写在 CMakePresets.json 里、没有任何 CMakeLists 读它：
# Debug 构建自称「with sanitizers」，而编译器命令行里什么都没有——对一个跑着事件循环、
# 线程池与异步数据库的工程来说，这是最不该存在的假安全。这里把它真正接上。
#
# 能力边界（决定了这里为什么不是一套参数打天下）：
# - MSVC 只提供 AddressSanitizer（/fsanitize=address），既无 UBSan 也无 TSan；
# - GCC/Clang 上顺带打开 UBSan（ASan 不覆盖未定义行为）；
# - 线程竞态检测（TSan）与 ASan 互斥、且 MSVC 不提供，需要在 Linux 侧单独配置构建。
option(ENABLE_SANITIZERS "启用 AddressSanitizer（GCC/Clang 上并含 UBSan）" OFF)

# 线程竞态检测（ThreadSanitizer）：与 ASan 互斥、MSVC 不提供，因此单列一个开关而不是塞进
# ENABLE_SANITIZERS。事件循环、线程池与跨循环投递（scheduleRemote/全局队列/TimerQueue 唤醒）
# 这些路径此前只有注释级约定，TSan 是唯一能自动兜住它们的工具；Linux CI 用本开关另起一个
# 构建目录跑并发相关的用例集（见 .github/workflows/linux-ci.yml）。
option(SANITIZE_THREADS "启用 ThreadSanitizer（仅 GCC/Clang；与 ENABLE_SANITIZERS 互斥）" OFF)

# 把 ASan 运行时拷到可执行文件旁边。
# MSVC 的 ASan 运行库不在系统 PATH 上（它随 Visual Studio 提供，位于 cl.exe 同目录），
# 因此插桩出来的可执行文件若在「没有 vcvars 环境」的裸 shell 里直接运行，会以
# 0xC0000135（STATUS_DLL_NOT_FOUND）启动失败——而这个错误不给任何可读提示，
# 只会让人以为「测试莫名跑不起来」。拷到输出目录即可让二进制自洽。
#
# 两份都拷：Debug 产物实测并不只依赖 _dbg_ 那一份（只拷 _dbg_ 仍会以同样的错误启动失败，
# 把两份补齐后正常）。两份 DLL 各自只有几百 KiB，为省这点体积去猜实际依赖不值得。
#
# 用法：在各个可执行目标定义之后调用 asyn_copy_sanitizer_runtime(<目标名>)。
function(asyn_copy_sanitizer_runtime target_name)
    if(NOT ENABLE_SANITIZERS OR NOT MSVC)
        return()
    endif()

    get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)

    foreach(runtime_name clang_rt.asan_dbg_dynamic-x86_64.dll clang_rt.asan_dynamic-x86_64.dll)
        add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${compiler_directory}/${runtime_name}"
                        "$<TARGET_FILE_DIR:${target_name}>"
                COMMENT "拷贝 AddressSanitizer 运行时（${runtime_name}）到 ${target_name} 输出目录")
    endforeach()
endfunction()

if(ENABLE_SANITIZERS)
    if(MSVC)
        # 运行库（clang_rt.asan_*_dynamic-x86_64.dll）由 MSVC 自带并位于 cl.exe 同目录，
        # 构建期无需额外处理；运行期由 asyn_copy_sanitizer_runtime() 拷到可执行文件旁
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)

        # MSVC 的 ASan 默认给 <vector>/<string>/<optional> 加上容器注解，并在目标文件里留下
        # annotate_vector / annotate_string / annotate_optional 三个 ABI 标记。Conan 提供的
        # gtest 没有用 ASan 编译（标记为 0），我们插桩后的对象标记为 1，链接会直接失败
        # （LNK2038「值不匹配」）。关掉注解即可对齐。代价与出路：
        # - 失去的是容器越界/迭代器失效这类注解检查；
        # - 堆、栈、use-after-free、溢出、双重释放等主要检测不受影响；
        # - 要在 MSVC 上恢复注解，得把 gtest 也用 /fsanitize=address 重建（Conan 侧注入
        #   cxxflags 重编整个依赖图），代价是第三方库全部插桩；
        # - GCC/Clang 无此问题（其 ASan 不加容器注解），因此注解只在 MSVC 分支关闭。
        add_compile_definitions(_DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION _DISABLE_OPTIONAL_ANNOTATION)

        message(STATUS "Sanitizers: 已启用 AddressSanitizer（MSVC 无 UBSan / TSan；"
                       "因 gtest 未插桩而关闭容器注解，见 CMakeLists 注释）")
    else()
        # -fno-omit-frame-pointer：栈回溯才有意义，否则报告里的调用栈会缺帧
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

    # -fno-omit-frame-pointer：竞态报告里的调用栈才有意义；-g 让报告能指回源码行
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
    message(STATUS "Sanitizers: 已启用 ThreadSanitizer（线程竞态检测；与 ASan 互斥）")
endif()

# Debug 构建注入 ASYN_DEBUG：日志输出线程号与源码位置，Release 走精简格式
add_compile_definitions($<$<CONFIG:Debug>:ASYN_DEBUG>)

enable_testing()

add_subdirectory(src)

# 两个开发者开关：默认全开（本地开发与 CI 都要它们），只有把本仓库打成对外分发包时才关。
# tests 需要 GTest、微基准与示例是纯粹的开发者产物，消费方不该为它们付出配置与构建代价。
# 关掉之后库本身与安装规则完全不受影响，因此这两个开关不改变任何对外行为。
option(ASYN_BUILD_TESTS "构建测试与热路径微基准" ON)
option(ASYN_BUILD_SAMPLES "构建示例程序" ON)

if(ASYN_BUILD_TESTS)
    add_subdirectory(tests)

    # 热路径微基准（HPACK 编解码、h1 解析器、h2 帧解码、HTTP 日期、request-id 生成）。
    # 与测试同开关：它量的是库的热路径，一旦不编译就会在库接口变化时悄悄失效。
    add_subdirectory(benchmarks/microbench)
endif()

if(ASYN_BUILD_SAMPLES)
    # 示例程序纳入构建：它的 CMakeLists 早已就绪，但此前从未被加入，示例与库的 API 脱节
    # 因此长期无人发现。纳入后示例随常规构建一起编译，任何时候都是可用的
    add_subdirectory(samples)
endif()

# ============================================================================
# 对外发布：安装规则与 CMake 包配置
# ============================================================================
# 五个模块库与公开头文件随安装落到 <前缀>/lib 与 <前缀>/include，并导出
# AsynGyanis::Platform / Base / Core / Net / Database（Windows 上还有 AsynGyanis::wepoll），
# 外部工程即可 find_package(AsynGyanis) 后直接链接，无需再自行拼 include 路径与依赖。
# 关闭后只保留纯构建行为：make install / cmake --install 不再产出任何东西。
option(ASYN_ENABLE_INSTALL "生成安装规则与 CMake 包配置，供外部工程 find_package 消费" ON)

if(ASYN_ENABLE_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS Platform Base Core Net Database
            EXPORT AsynGyanisTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # wepoll 是 Core 在 Windows 上链接的 vendored 静态库，必须一并导出：
    # 少了它，消费方链接 Core 时会缺 epoll_create/epoll_wait 等符号
    if(TARGET wepoll)
        install(TARGETS wepoll
                EXPORT AsynGyanisTargets
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()

    # 头文件保持 src 下的相对路径，安装后 "Core/Socket/AsyncSocket.h" 这类
    # 库内既有写法仍可直接使用（vendored 的 wepoll.h 也随此规则一起落地）
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.h"
    )

    # 消费方同样需要的外部依赖：各模块启用依赖时把自己登记进 GLOBAL 属性
    # （imported 目标只在创建它的目录及其子目录可见，根作用域 here 看不见，
    #  所以不能在本文件里用 if(TARGET ...) 反查），这里统一转成 find_dependency
    get_property(ASYN_PACKAGE_DEPENDENCIES GLOBAL PROPERTY AsynGyanisPackageDependencies)
    set(ASYN_CONFIG_DEPENDENCIES "")
    foreach(dependency_name IN LISTS ASYN_PACKAGE_DEPENDENCIES)
        string(APPEND ASYN_CONFIG_DEPENDENCIES "find_dependency(${dependency_name})\n")
    endforeach()

    configure_package_config_file(
            ${CMAKE_SOURCE_DIR}/cmake/AsynGyanisConfig.cmake.in
            ${CMAKE_CURRENT_BINARY_DIR}/AsynGyanisConfig.cmake
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AsynGyanis
    )

    # 版本号取自 project()，同一主版本内视为兼容
    write_basic_package_version_file(
            ${CMAKE_CURRENT_BINARY_DIR}/AsynGyanisConfigVersion.cmake
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion
    )

    install(EXPORT AsynGyanisTargets
            FILE AsynGyanisTargets.cmake
            NAMESPACE AsynGyanis::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AsynGyanis
    )

    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/AsynGyanisConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/AsynGyanisConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AsynGyanis
    )
endif()