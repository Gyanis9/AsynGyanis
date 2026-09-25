# 随仓库一起构建的第三方源码。只放「ConanCenter 拿不到、又非它不可」的库，
# 能走 ConanCenter 的一律走 Conan（见根目录 conandata.yml）。
#
# 当前这份清单是**空的**，两条历史 vendor 的去向记在这儿，免得有人再把它们请回来：
# - ngtcp2：曾是 QUIC 传输层的测试裁判（库本体 2026-09 已自研）。跨实现验收换成进程外的
#   aioquic（scripts/quic_cross_check.sh + tests/Tools/QuicProbeServer.cpp），不再链接任何
#   第三方 QUIC 实现——同树互测暴露不出共享的误解，还会跟着被测代码一起被改松。
# - nghttp3：曾是 HTTP/3 帧与 QPACK 的测试裁判（这两层已全自研）。同一原因退出：h3 会话用例的对端
#   换成测试自己按 RFC 9114/9204 排字节，字节级一致性判定交给进程外的 aioquic 探针
#   （scripts/h3_acceptance.py）。QPACK 的字节向量另有 RFC 9204 附录 B/C 与 pylsqpack 产出的对照
#   输入（见 tests/Net/Http3/TestQpack.cpp 的说明），不依赖任何链接进来的实现。
# - wepoll：现代改写后并入 Platform，源码已删（只留 wepoll.h 供 Iocp.h 借类型定义）。
#
# 要给引擎加第三方库时：先确认 ConanCenter 没有配方，再在这里登记，并在
# packaging/conan/conanfile.py 的导出清单里带上对应源（漏了它，打包路线在配置期就失败）。
