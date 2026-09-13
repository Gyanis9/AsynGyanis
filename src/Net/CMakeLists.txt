# ============================================================================
# Net 模块 — HTTP/HTTPS 应用层与 TCP 服务框架
# ============================================================================
# Http  报文解析、请求/响应对象、会话与服务器、路由与中间件、静态文件 MIME
# Tcp   传输层骨架：监听套接字、带缓冲字节流、服务器基类
# WebSocket  RFC 6455 握手计算/校验、帧编解码与升级后的对端收发对象
# Http2  RFC 7540/7541 帧编解码、HPACK 头块压缩、连接层状态机与会话（ALPN 协商出 h2 时使用）
# 分层构建：叶子目录只登记本层源文件，逐层汇总后产出一个静态库
# ============================================================================

# HTTP 报文解析由 Http/HttpParser 自己实现（手写状态机），不再依赖外部解析库
set_property(GLOBAL PROPERTY AsynGyanisNetSources "")

add_subdirectory(Http)
add_subdirectory(Tcp)
add_subdirectory(WebSocket)
add_subdirectory(Http2)

get_property(NET_SOURCES GLOBAL PROPERTY AsynGyanisNetSources)

add_library(Net STATIC
        ${NET_SOURCES}
)

# 响应压缩用 zlib（gzip 容器，RFC 1952）；依赖由 Conan 提供，本地缓存里已有
find_package(ZLIB REQUIRED)

# Net 建立在 Core 的事件循环与协程 I/O 之上；Platform 与 Base 由 Core 传递而来
target_link_libraries(Net PUBLIC
        Core
)

# PRIVATE 就够了：zlib 只出现在 Gzip.cpp 里；静态库会把这份依赖以 LINK_ONLY 传给最终可执行文件
target_link_libraries(Net PRIVATE ZLIB::ZLIB)

target_include_directories(Net PUBLIC
        # 构建期用仓库内的 src；安装后改用 <前缀>/include，两者都是各模块头文件的根
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(Net PUBLIC cxx_std_20)
