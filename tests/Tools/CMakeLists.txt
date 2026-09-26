# tests/Tools —— 测试期可执行体：不是用例，是给外部工具当对手或夹具的小程序
#
# 与 tests/<模块>/ 的区别：那边是「与 src 逐级对应的 gtest 源」，这里放**独立进程形态的夹具**。
# 裁判换成这些工具 + 外部实现之后，判据不再由本仓库的代码给出，改代码时也就改不动判据。

# QUIC 跨实现校验探针的被测端：把 QuicServer 的传输层行为打成 stdout 事件行，
# 由 scripts/quic_cross_check.py（aioquic）与 scripts/quic_cross_check.sh 消费
add_executable(quic_probe_server
        QuicProbeServer.cpp)

target_link_libraries(quic_probe_server PRIVATE Net Core Base Platform)

# QUIC 出站跨实现校验探针的被测端：把 QuicClientConnection 的传输行为打成 stdout 事件行，
# 由 scripts/quic_outbound_cross_check.py（aioquic 起服务端）与同名 .sh 消费
add_executable(quic_probe_client
        QuicProbeClient.cpp)

target_link_libraries(quic_probe_client PRIVATE Net Core Base Platform)
