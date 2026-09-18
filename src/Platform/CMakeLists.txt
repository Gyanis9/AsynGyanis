# Platform 分层构建：叶子目录只登记本层源文件，逐层汇总后产出一个静态库。
# 源列表通过 GLOBAL 属性累加，使任意嵌套深度都能向上传递且不受目录变量作用域影响。
# 子目录仅作物理组织，命名空间统一为 AsynGyanis::Platform。
set_property(GLOBAL PROPERTY AsynGyanisPlatformSources "")

add_subdirectory(System)
add_subdirectory(IO)
add_subdirectory(FileSystem)

get_property(PLATFORM_SOURCES GLOBAL PROPERTY AsynGyanisPlatformSources)

add_library(Platform STATIC
        ${CMAKE_CURRENT_LIST_DIR}/Platform.h
        ${PLATFORM_SOURCES}
)

# 网络子系统仅在 Windows 需要 Winsock 库，Linux 侧由 libc 内建提供
if(WIN32)
    target_link_libraries(Platform PUBLIC
            ws2_32
            Mswsock
    )
endif()

find_package(Threads REQUIRED)

# 登记随包导出的外部依赖（按组件记）：消费方链接本模块时也要能找到 Threads::Threads
set_property(GLOBAL APPEND PROPERTY AsynGyanisPackageDependencies_Platform Threads)

target_link_libraries(Platform PUBLIC
        Threads::Threads
)

target_include_directories(Platform PUBLIC
        # 构建期用仓库内的 src；安装后改用 <前缀>/include，两者都是各模块头文件的根
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(Platform PUBLIC cxx_std_23)
