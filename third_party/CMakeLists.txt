# 随仓库一起构建的第三方源码。只放「ConanCenter 拿不到、又非它不可」的库，
# 能走 ConanCenter 的一律走 Conan（见根目录 conandata.yml）。
#
# 当前这份清单是**空的**，两条历史 vendor 的去向记在这儿，免得有人再把它们请回来：
# - ngtcp2：曾是 QUIC 传输层的测试裁判（库本体 2026-09 已自研）。跨实现验收换成进程外的
#   aioquic（scripts/quic_cross_check.sh + tests/Tools/QuicProbeServer.cpp），不再链接任何
#   第三方 QUIC 实现——同树互测暴露不出共享的误解，还会跟着被测代码一起被改松。
# - nghttp3：HTTP/3 帧与 QPACK 的旧裁判，正按同一原因退出（帧层与 QPACK 已全自研，
#   对外一致性由 aioquic 的 h3 探针 scripts/h3_acceptance.py 钉）。
# - wepoll：现代改写后并入 Platform，源码已删（只留 wepoll.h 供 Iocp.h 借类型定义）。
#
# 要给引擎加第三方库时：先确认 ConanCenter 没有配方，再在这里登记，并在
# packaging/conan/conanfile.py 的导出清单里带上对应源（漏了它，打包路线在配置期就失败）。
