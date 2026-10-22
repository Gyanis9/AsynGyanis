#!/bin/bash
# h3 服务端系统调用画像：固定负载下数每种收发的调用次数，作 A/B 守护读数，不看计时。
# 用法：h3_syscall_ab.sh <构建目录> <标签> [额外的 ReferenceServer 参数]

# 数调用而不是数吞吐：aioquic 客户端自身是瓶颈（约 1000 请求/6 秒），它的 rps 判收益等于读噪声。
# 系统调用次数是确定量，同一输入两次同数，才配当「这条守护能不能检出回归」的判据。

# 首测（ubuntu24 容器、build-asan、4 连接 × 25 请求、--threads 1）：
# epoll_wait 41913 / recvfrom 445（37 次 EAGAIN）/ sendto 402 / recvmsg 2，
# 数据面每请求约 4.5 次读、4 次写，逐条 recvfrom，没有批量。

# epoll_wait 四万次不是自旋：QUIC 服务端有 10 毫秒的 expiry ticker
# （QuicServer::Configuration::expiryTickInterval），每次唤醒约两趟 epoll_wait；
# strace 把整场压到三分多钟，200 次/秒 × 200 秒 ≈ 四万。

# 空闲开销由节拍主导，与负载无关：降它要把固定节拍轮询换成按最早的交易截止时间睡，
# 加 recvmmsg 批量并不能碰它一分。

# 改完同负载复测：epoll_wait 2126、recvfrom 417、sendto 379，唤醒少 95%，数据面两次调用几乎没动。
# 两侧都过得了用例，只有这份画像分得出差别，这就是它当守护读数的理由。

# 一次唤醒里抽干已到报文经实测否决：抽干前 epoll_wait 83 / recvfrom 416（32 次 EAGAIN）/
# sendto 378 / 合计 877；抽干后 82 / 447（63 次 EAGAIN）/ 378 / 907。
# 唤醒一分没省，多出来的是每批一次空探的系统调用。

# 真要减读数只能上 recvmmsg，要求每槽位按整条报文上限（四条一批 256 KiB 常驻每条监听循环）；
# 按这份画像折算每请求省不到 1 µs，不值这份内存与平台分叉。

# 实测坑一：不能用 strace -p 挂已存在的进程（容器不给 PTRACE_SEIZE，Operation not permitted），
# 服务必须由 strace 带着起、由服务自己收口（SIGINT）。

# 实测坑二：认进程按可执行体名（pgrep -x ReferenceServer），不用 pgrep -f。
# -f 会先命中 strace 自己的命令行，SIGINT 只是让它 detach，服务留在场上，脚本卡在 wait。
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
pkill -x ReferenceServer 2>/dev/null
pkill -x strace 2>/dev/null
sleep 1

started_at=$SECONDS
strace -f -c -e trace=recvfrom,recvmmsg,recvmsg,sendto,sendmsg,writev,epoll_wait \
  -o "$log_dir/$label-strace.txt" \
  "$build_dir/samples/ReferenceServer" --host 127.0.0.1 --port "$port" --https --h3 \
  --cert tests/Core/fixtures/test_cert.pem --key tests/Core/fixtures/test_key.pem $extra_flags \
  > "$log_dir/$label-server.txt" 2>&1 &

for _ in $(seq 1 60); do
  grep -qa "starting" "$log_dir/$label-server.txt" && break
  sleep 0.2
done
sleep 1
server_pid="$(pgrep -x ReferenceServer | head -1)"

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
