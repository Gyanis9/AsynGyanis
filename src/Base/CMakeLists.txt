# Base 分层构建：叶子目录只登记本层源文件，逐层汇总后产出一个静态库。
# 源列表通过 GLOBAL 属性累加，使任意嵌套深度都能向上传递且不受目录变量作用域影响。
# 子目录仅作物理组织，命名空间统一为 AsynGyanis::Base。
set_property(GLOBAL PROPERTY AsynGyanisBaseSources "")

find_package(nlohmann_json 3.12.0 CONFIG REQUIRED)
find_package(yaml-cpp 0.9.0 CONFIG REQUIRED)
set_property(GLOBAL APPEND PROPERTY AsynGyanisPackageDependencies_Base nlohmann_json yaml-cpp)

add_subdirectory(Exception)
add_subdirectory(Config)
add_subdirectory(Log)

get_property(BASE_SOURCES GLOBAL PROPERTY AsynGyanisBaseSources)

add_library(Base STATIC
        ${BASE_SOURCES}
)

# 可选能力探测：std::stacktrace（异常与日志的调用栈）。GCC 13/14 的 libstdc++ 需要额外链接
# stdc++exp，探测不过就退化为空栈（见 Exception/StackTrace.h 的降级实现），不让 configure 失败
include(CheckCXXSourceCompiles)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
endif()
check_cxx_source_compiles("
#include <stacktrace>
int main()
{
    return static_cast<int>(std::stacktrace::current().size());
}
" ASYN_HAS_STACKTRACE)
unset(CMAKE_REQUIRED_LIBRARIES)
if(ASYN_HAS_STACKTRACE)
    message(STATUS "Base: std::stacktrace 可用，异常与日志将携带调用栈")
    target_compile_definitions(Base PUBLIC ASYN_HAS_STACKTRACE=1)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_libraries(Base PUBLIC stdc++exp)
    endif()
else()
    message(STATUS "Base: std::stacktrace 不可用，调用栈退化为空实现")
endif()

target_link_libraries(Base PUBLIC
        Platform
        nlohmann_json::nlohmann_json
        yaml-cpp::yaml-cpp
)

target_include_directories(Base PUBLIC
        # 构建期用仓库内的 src；安装后改用 <前缀>/include，两者都是各模块头文件的根
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(Base PUBLIC cxx_std_23)
