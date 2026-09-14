# 随仓库一起构建的第三方源码。只放「ConanCenter 拿不到、又非它不可」的库，
# 能走 ConanCenter 的一律走 Conan（见根目录 conandata.yml）。

# ---------------------------------------------------------------------------
# ngtcp2 —— QUIC 传输层（HTTP/3 的底座）
#   来源：https://github.com/ngtcp2/ngtcp2 的 v1.25.0 发布包（codeload tar.gz）
#   裁剪：保留构建库所需——CMakeLists.txt、CMakeOptions.txt、cmakeconfig.h.in、cmake/、
#         lib/、crypto/、third-party/（被无条件 add_subdirectory）、README.rst（含 MIT 许可）、
#         NEWS；去掉 examples/、tests/、fuzz/、doc/、m4/、ci/、docker/、interop/
#   许可：MIT（见 ngtcp2/README.rst 的 License 段）
#   为什么 vendor：ConanCenter 没有 ngtcp2 配方（2026-09-14 实测 0 条），
#                 而 HTTP/3 的非 TLS 部分（帧与 QPACK）用 ConanCenter 的 nghttp3/1.12.0
# ---------------------------------------------------------------------------

# 只要库：examples 与 tests 会拖进 libev、nghttp3 等与库无关的依赖
set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)

# ngtcp2 用 check_symbol_exists 探测 OpenSSL 有没有 QUIC API，而它既把 ${OPENSSL_LIBRARIES} 交给
# try_compile（在 Conan 下那是 IMPORTED 目标名，独立工程认不出，报 CONAN_LIB::… 找不到），
# 那一步本身还要链接才能成立。这里换成**版本判据**：主线 OpenSSL 自 3.5 起提供
# SSL_set_quic_tls_cbs（QUIC TLS 回调 API），3.5 以下只有 quictls 分支才有；
# 于是把结论写成 ngtcp2 会读的缓存变量，它的同名探测见到变量已定义就跳过。
# 判据随 Conan 里的 openssl 版本变化，不是写死的「有」。
find_package(OpenSSL REQUIRED)
# 本目录的库是 C 写的：先把 C 启起来（根工程只声明了 CXX），否则下面 add_subdirectory 里的 .c 无从编译
enable_language(C)

if(NOT DEFINED OPENSSL_VERSION OR OPENSSL_VERSION VERSION_LESS 3.5)
    message(FATAL_ERROR "ngtcp2 的 OpenSSL 后端需要 3.5+ 的主线 OpenSSL（QUIC TLS 回调 API）或 quictls；"
                        "当前 OPENSSL_VERSION=\"${OPENSSL_VERSION}\"。请升级 Conan 依赖里的 openssl")
endif()
set(HAVE_SSL_SET_QUIC_TLS_CBS 1 CACHE INTERNAL "主线 OpenSSL 3.5+ 自带 SSL_set_quic_tls_cbs")
set(HAVE_SSL_PROVIDE_QUIC_DATA 0 CACHE INTERNAL "主线 OpenSSL 没有 quictls 专有的 SSL_provide_quic_data")

# EXCLUDE_FROM_ALL：没有目标链接它时不必编译（第三方源码不该拖慢默认构建），
# 被 QUIC 相关目标链接之后自然进入构建图。
# BUILD_TESTING 用**目录作用域**的正常变量压成 OFF：缓存里它是 ON（GTest 那条依赖链带进来的），
# 而 ngtcp2 的 if(BUILD_TESTING) 会去 add_subdirectory(tests)——tests/ 已从 vendored 副本里去掉
# （它要拖进 GTest、libev 等与库无关的东西）。正常变量优先于缓存，因此只影响 ngtcp2 这一支
set(BUILD_TESTING OFF)
add_subdirectory(ngtcp2 EXCLUDE_FROM_ALL)
