# Net 模块：HTTP/HTTPS 应用层与 TCP 服务框架。Http 解析/会话/路由/中间件，Tcp 监听与字节流，
# WebSocket（RFC 6455），Http2（RFC 9113/7541）、Tracing（OTLP）与 Acme（RFC 8555）；
# 叶子目录只登记本层源文件，逐层汇总后产出一个静态库。

# HTTP 报文解析为 Http/HttpParser 的手写状态机，不依赖外部解析库。
set_property(GLOBAL PROPERTY AsynGyanisNetSources "")

add_subdirectory(Http)
add_subdirectory(Tcp)
add_subdirectory(Udp)
add_subdirectory(Proxy)
add_subdirectory(WebSocket)
add_subdirectory(Http2)
add_subdirectory(Quic)
add_subdirectory(Http3)
add_subdirectory(Tracing)
add_subdirectory(Telemetry)
add_subdirectory(Metrics)
add_subdirectory(Logging)
add_subdirectory(Acme)

get_property(NET_SOURCES GLOBAL PROPERTY AsynGyanisNetSources)

add_library(Net ${ASYN_LIBRARY_KIND}
        ${NET_SOURCES}
)
# 静态是默认形态；打开 BUILD_SHARED_LIBS 时本模块要带上自己的导出宏定义与可见性预设
asyn_configure_module_library(Net)

# 响应压缩：zlib（gzip，RFC 1952）、zstd（RFC 8878）与 brotli（RFC 7932）。
find_package(ZLIB REQUIRED)

find_package(zstd REQUIRED)
find_package(brotli REQUIRED)

# 静态库把 PRIVATE 链接的压缩库以 $<LINK_ONLY:...> 传给最终可执行文件，消费方同样要能找到
# 这些目标；漏登记会在消费方 generate 阶段以「目标未定义」失败。
set_property(GLOBAL APPEND PROPERTY AsynGyanisPackageDependencies_Net ZLIB zstd brotli)

# Net 建立在 Core 的事件循环与协程 I/O 之上；Platform 与 Base 由 Core 传递而来
target_link_libraries(Net PUBLIC
        Core
)

# PRIVATE 够用：压缩库只出现在 Gzip.cpp / Compression.cpp 里，静态库以 LINK_ONLY 传给可执行文件。
target_link_libraries(Net PRIVATE ZLIB::ZLIB zstd::libzstd brotli::brotli)

# QUIC 传输层自研，不链接第三方 QUIC；跨实现验收走进程外的 aioquic（scripts/quic_cross_check.sh
# + tests/Tools/QuicProbeServer.cpp）。OpenSSL 仍为 PUBLIC：连接与服务端配置直接暴露 SSL_CTX，
# 这里显式 find_package 一次，依赖关系不随模块顺序改变而失效。
find_package(OpenSSL REQUIRED)
target_link_libraries(Net PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# HTTP/3 协议层（帧、QPACK、连接状态机、会话）同样自研，测试对端是自带的字节级实现，
# 因此包消费方不必为 h3 声明任何第三方依赖。

target_include_directories(Net PUBLIC
        # 构建期用仓库内的 src；安装后改用 <前缀>/include，两者都是各模块头文件的根
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(Net PUBLIC cxx_std_23)
