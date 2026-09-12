# ============================================================================
# Net 模块 — HTTP/HTTPS 应用层与 TCP 服务框架
# ============================================================================
# Http  报文解析、请求/响应对象、会话与服务器、路由与中间件、静态文件 MIME
# Tcp   传输层骨架：监听套接字、带缓冲字节流、服务器基类
# 分层构建：叶子目录只登记本层源文件，逐层汇总后产出一个静态库
# ============================================================================

# HTTP 报文解析依赖 llhttp 的状态机
find_package(llhttp REQUIRED)

set_property(GLOBAL PROPERTY AsynGyanisNetSources "")

add_subdirectory(Http)
add_subdirectory(Tcp)

get_property(NET_SOURCES GLOBAL PROPERTY AsynGyanisNetSources)

add_library(Net STATIC
        ${NET_SOURCES}
)

# Net 建立在 Core 的事件循环与协程 I/O 之上；Platform 与 Base 由 Core 传递而来
target_link_libraries(Net PUBLIC
        Core
        llhttp::llhttp
)

target_include_directories(Net PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(Net PUBLIC cxx_std_20)
