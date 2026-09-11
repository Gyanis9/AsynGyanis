# Base 分层构建：叶子目录只登记本层源文件，逐层汇总后产出一个静态库。
# 源列表通过 GLOBAL 属性累加，使任意嵌套深度都能向上传递且不受目录变量作用域影响。
# 子目录仅作物理组织，命名空间统一为 AsynGyanis::Base。
set_property(GLOBAL PROPERTY AsynGyanisBaseSources "")

add_subdirectory(Exception)
add_subdirectory(Parser)
add_subdirectory(Config)
add_subdirectory(Log)

get_property(BASE_SOURCES GLOBAL PROPERTY AsynGyanisBaseSources)

add_library(Base STATIC
        ${BASE_SOURCES}
)

target_link_libraries(Base PUBLIC
        Platform
        yaml-cpp::yaml-cpp
)

if(TARGET jemalloc::jemalloc)
    target_link_libraries(Base PUBLIC jemalloc::jemalloc)
endif()

target_include_directories(Base PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(Base PUBLIC cxx_std_20)
