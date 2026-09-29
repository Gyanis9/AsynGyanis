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

# 监听套接字跨进程移交的 worker 侧夹具：认编排器追加的 --handed-over-listener，取回移交来的监听引用
# 后接受连接并回一行文本。由 tests/Core/Process/TestWorkerSupervisor.cpp 当子进程派生——Windows 的
# 多进程编排只有真的另起一个进程才测得到，WSADuplicateSocketW 是按目标进程号发凭证的
add_executable(handoff_worker
        HandoffWorker.cpp)

target_link_libraries(handoff_worker PRIVATE Core Base Platform)

# ACME 真签发探针的被测端：对着 Pebble（Let's Encrypt 官方那套 ACME 测试服务端，Go 写的独立实现）
# 把整台状态机走到「签出一张链」，只把每步结果打成 stdout 事件行。判据刻意不在本仓库这一侧：
# 接受不接受我们的 JWS 与 CSR、签不签得出证书，都由对面说了算
add_executable(acme_issuance_probe
        AcmeIssuanceProbe.cpp)

target_link_libraries(acme_issuance_probe PRIVATE Net Core Base Platform)
