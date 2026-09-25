# - Find brotli：把两种互不相同的导出名收成 brotli::brotli 这一个形状
#
# 为什么要这一层：brotli 的 CMake 导出名不统一。上游自带的配置（本仓库的 Conan 路线用的正是它）
# 给出 brotli::brotli / brotli::brotlicommon 一族；而 vcpkg 的 brotli 端口刻意只导出
# 「unofficial-brotli」这个包名与 unofficial::brotli::{brotlicommon,brotlidec,brotlienc} 三个目标，
# 并在自己的 usage 里写明「请调用方包一层」。与其为其中一个包管理器改掉另一条路线的链接名，
# 不如把适配收在这一个模块里：两条路线的产物逐字节相同，差的只是名字。
#
# 生效顺序：调用方写 find_package(brotli REQUIRED)（不带 CONFIG）时模块模式先走到这里；
# 本模块先按**配置模式**找真 brotli——Conan 与上游都把配置放在 CMAKE_PREFIX_PATH / brotli_DIR
# 能看见的地方——找到就直接交出去，行为与没有本模块时逐字节一致。找不到才退到 vcpkg 那套名字。
# 两条都没有时报的是「brotli 找不到」，不能报成「unofficial-brotli 找不到」：调用方问的是前者，
# 把后者的名字抛出去会让人以为必须换包管理器（这是本模块自己踩过的坑）。

find_package(brotli CONFIG QUIET)

if(TARGET brotli::brotli)
    set(brotli_FOUND TRUE)
    set(Brotli_FOUND TRUE)
    return()
endif()

find_package(unofficial-brotli CONFIG QUIET)

if(TARGET unofficial::brotli::brotlienc)
    # 逐个补齐：只建缺的那几个，重复 find_package 不会把已存在的导入目标再建一遍
    foreach(_asynBrotliComponent IN ITEMS brotlicommon brotlidec brotlienc)
        if(NOT TARGET brotli::${_asynBrotliComponent})
            add_library(brotli::${_asynBrotliComponent} INTERFACE IMPORTED)
            set_target_properties(brotli::${_asynBrotliComponent} PROPERTIES
                    INTERFACE_LINK_LIBRARIES unofficial::brotli::${_asynBrotliComponent})
        endif()
    endforeach()

    if(NOT TARGET brotli::brotli)
        add_library(brotli::brotli INTERFACE IMPORTED)
        set_target_properties(brotli::brotli PROPERTIES
                INTERFACE_LINK_LIBRARIES "brotli::brotlicommon;brotli::brotlidec;brotli::brotlienc")
    endif()

    unset(_asynBrotliComponent)

    set(brotli_FOUND TRUE)
    set(Brotli_FOUND TRUE)
    return()
endif()

message(FATAL_ERROR
        "找不到 brotli：既没有导出 brotli::brotli 的包配置（上游 CMake 与 Conan 的形状），"
        "也没有 vcpkg 的 unofficial-brotli。请确认依赖已装好，且 CMAKE_PREFIX_PATH 或 brotli_DIR "
        "指向它提供的 cmake 目录。")
