# - Find brotli：把两种互不相同的导出名收成 brotli::brotli 这一个形状
#
# 为什么要这一层：brotli 的 CMake 导出名不统一。上游自带的配置（本仓库的 Conan 路线用的正是它）
# 给出 brotli::brotli / brotli::brotlicommon 一族；而 vcpkg 的 brotli 端口刻意只导出
# 「unofficial-brotli」这个包名与 unofficial::brotli::{brotlicommon,brotlidec,brotlienc} 三个目标，
# 并在自己的 usage 里写明「请调用方包一层」。与其为其中一个包管理器改掉另一条路线的链接名，
# 不如把适配收在这一个模块里：两条路线的产物逐字节相同，差的只是名字。
#
# 生效顺序天然安全：vcpkg 的工具链把「配置优先」打开了，真正的 brotli 配置在时根本走不到这里；
# 走到这里说明确实没有 brotli 配置，此时再退到 unofficial-brotli，并且 REQUIRED——
# 两条都没有时报的就是这个错，不必再拼一句更委婉的话。

find_package(brotli CONFIG QUIET)

if(TARGET brotli::brotli)
    set(brotli_FOUND TRUE)
    set(Brotli_FOUND TRUE)
    return()
endif()

find_package(unofficial-brotli CONFIG REQUIRED)

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
