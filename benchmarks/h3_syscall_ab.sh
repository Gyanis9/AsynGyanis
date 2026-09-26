#!/bin/bash
# h3 服务端的系统调用画像：固定负载下数每种收发的调用次数（A/B 的守护读数，不看计时）
# 用法：h3_syscall_ab.sh <构建目录> <标签> [额外的 echo_server 参数]
#
# 为什么数调用次数而不是数吞吐：aioquic 那个 Python 客户端自己就是瓶颈（每条连接稳定在
# ~1000 请求/6 秒），拿它的 rps 判服务端改动的收益等于读噪声。系统调用次数是确定量，同一份
# 输入两次跑出同一个数，才配当「这条守护能不能检出回归」的判据。
#
# 2026-09-26 首测读数（ubuntu24 容器、build-asan、4 连接 × 25 请求、--threads 1）：
#   epoll_wait 41913 / recvfrom 445（37 次 EAGAIN）/ sendto 402 / recvmsg 2
#   —— 数据面每请求约 4.5 次读、4 次写，逐条 recvfrom，没有批量
#   —— epoll_wait 那四万次不是自旋：QUIC 服务端有一个 10 毫秒的 expiry ticker
#      （QuicServer::Configuration::expiryTickInterval），每次唤醒约两趟 epoll_wait，
#      而 strace 把整场压到三分多钟，于是 200 次/秒 × 200 秒 ≈ 四万。**空闲开销由节拍主导，
#      与负载无关**：要降它得把「固定节拍轮询」换成「按最早的交易截止时间睡」，
#      加 recvmmsg 批量并不能碰它一分
#
#   改完再跑同一条负载（`--threads 1`，同一台容器、同一棵树）：epoll_wait 2126、
#   recvfrom 417、sendto 379 —— 唤醒少了 95%，数据面两次调用几乎没动，省下的确实都是空转。
#   这就是这条画像当守护读数的理由：两侧都过得了用例，只有它能分出差别。
#
# 两处实测出来的坑，别绕过：
#   ① 不能用 `strace -p` 挂到已存在的进程上——这个容器不给 PTRACE_SEIZE 权限
#      （"ptrace(PTRACE_SEIZE, ...): Operation not permitted"），只有随 strace 一起 fork
#      出来的子进程能被跟。于是服务必须由 strace 带着起、由服务自己收口（SIGINT）。
#   ② 认进程要按**可执行体名**（pgrep -x echo_server）而不是命令行（pgrep -f）：strace 自己的
#      命令行里就带着 echo_server 的路径，-f 会先命中它，SIGINT 送给 strace 只是让它 detach，
#      服务留在场上没人收，脚本卡在 wait 上（实测卡过两次，留了一排僵尸）。
set -u
build_dir="${1:-/root/ticketgate/build-asan}"
label="${2:-baseline}"
shift 2 2>/dev/null || true
extra_flags="$*"
port=18643
log_dir=/tmp/h3ab
mkdir -p "$log_dir"
cd /root/ticketgate || exit 1

# 上一轮没清干净就先清掉（按可执行体名，别用 -f：那会连这个脚本自己一起匹配到）
pkill -x echo_server 2>/dev/null
pkill -x strace 2>/dev/null
sleep 1

started_at=$SECONDS
strace -f -c -e trace=recvfrom,recvmmsg,recvmsg,sendto,sendmsg,writev,epoll_wait \
  -o "$log_dir/$label-strace.txt" \
  "$build_dir/samples/echo_server" --host 127.0.0.1 --port "$port" --https --h3 \
  --cert tests/Core/fixtures/test_cert.pem --key tests/Core/fixtures/test_key.pem $extra_flags \
  > "$log_dir/$label-server.txt" 2>&1 &

for _ in $(seq 1 60); do
  grep -qa "starting" "$log_dir/$label-server.txt" && break
  sleep 0.2
done
sleep 1
# 认进程要按**可执行体名**而不是命令行：`pgrep -f samples/echo_server` 会先命中 strace 自己
# （它的命令行里就带着这个路径），SIGINT 送给 strace 只是让它 detach，服务留在场上没人收
server_pid="$(pgrep -x echo_server | head -1)"

/root/h3venv/bin/python benchmarks/h3_soak.py --host 127.0.0.1 --port "$port" \
  --connections 4 --requests-per-connection 25 --tunnels 0 --skip-compression --skip-idle-reap \
  > "$log_dir/$label-soak.txt" 2>&1
soak_exit=$?
sleep 1

kill -INT "$server_pid"
wait
echo "SOAK_EXIT=$soak_exit LABEL=$label FLAGS=[$extra_flags] SERVER_PID=$server_pid ELAPSED_SECONDS=$((SECONDS - started_at))"
tail -2 "$log_dir/$label-soak.txt"
echo "----- syscall summary ($label) -----"
grep -aE "epoll_wait|recvfrom|recvmmsg|sendto|sendmsg|recvmsg|writev|total" "$log_dir/$label-strace.txt" | tail -10
