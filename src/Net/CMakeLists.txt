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
add_subdirectory(Quic)
add_subdirectory(Http3)

get_property(NET_SOURCES GLOBAL PROPERTY AsynGyanisNetSources)

add_library(Net STATIC
        ${NET_SOURCES}
)

# 响应压缩用 zlib（gzip 容器，RFC 1952）；依赖由 Conan 提供，本地缓存里已有
find_package(ZLIB REQUIRED)

# 压缩的另外两种算法：zstd（RFC 8878）与 brotli（RFC 7932）
find_package(zstd REQUIRED)
find_package(brotli REQUIRED)

# 登记随包导出的外部依赖（按组件记）：静态库把 PRIVATE 链接的压缩库以 $<LINK_ONLY:...>
# 传给最终可执行文件，消费方因此同样要能找到这些目标——漏登记会在消费方 generate 阶段
# 就以「目标未定义」失败，而不是等到链接期
set_property(GLOBAL APPEND PROPERTY AsynGyanisPackageDependencies_Net ZLIB zstd brotli)

# Net 建立在 Core 的事件循环与协程 I/O 之上；Platform 与 Base 由 Core 传递而来
target_link_libraries(Net PUBLIC
        Core
)

# PRIVATE 就够了：压缩库只出现在 Gzip.cpp / Compression.cpp 里；静态库会把这份依赖
# 以 LINK_ONLY 传给最终可执行文件（消费方的可见性由上面的清单登记保证）
target_link_libraries(Net PRIVATE ZLIB::ZLIB zstd::libzstd brotli::brotli)

# QUIC 传输层已是自研实现，本模块不链接任何第三方 QUIC：跨实现验收换成进程外的 aioquic
# （scripts/quic_cross_check.sh + tests/Tools/QuicProbeServer.cpp），树内不再留裁判链接。
# OpenSSL 仍是 PUBLIC：连接与服务端的配置里直接暴露 SSL_CTX，消费方 include 这些头就要拿到
# OpenSSL 的头路径。这里自己 find_package 一次：OpenSSL 目前是靠 Core 带进来的，显式声明过
# 依赖关系才不会随模块顺序改变而失效
find_package(OpenSSL REQUIRED)
target_link_libraries(Net PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# HTTP/3 协议层（帧、QPACK、连接状态机、会话）已全部自研，Net 不再链接任何第三方 h3 实现。
# nghttp3 只剩测试依赖：它当跨实现裁判出现在 tests/Net/CMakeLists.txt 里，
# 因此包消费方（导出包、Conan 的 net 组件）都不必再为它 find_package 或声明依赖。

target_include_directories(Net PUBLIC
        # 构建期用仓库内的 src；安装后改用 <前缀>/include，两者都是各模块头文件的根
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(Net PUBLIC cxx_std_23)
