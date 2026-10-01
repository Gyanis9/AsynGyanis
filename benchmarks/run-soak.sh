#!/usr/bin/env bash
#
# 进程外压测的 Linux 侧驱动：起服务端 → 跑压测脚本 → 收进程，与 benchmarks/run-soak.bat 同一件事。
#
# 为什么补这一件：性能门禁此前只有 Windows 的 .bat，基线也就只能在一台机器上录。
# 而部署与 CI 都在 Linux 上——那份快照对 Linux 的任何改动都判不出东西。
# 口径不变：基线只对「同一台机器 + 同一个构建配置」有意义，所以 Linux 侧用自己那份
# benchmarks/baseline-linux.json，比对时显式 --baseline 指过去，不与 Windows 那份互比。
#
# 用法（服务端由本脚本起，跑完收掉；额外参数原样转给对应的压测脚本）：
#   ASYN_SOAK_BUILD=release benchmarks/run-soak.sh 18080 4 --json-out build/soak-r1.json
#   ASYN_SOAK_BUILD=release ASYN_SOAK_STAGE=h2c benchmarks/run-soak.sh 18081 4 --pipeline 32 --json-out ...
#   ASYN_SOAK_BUILD=release ASYN_SOAK_STAGE=h3  benchmarks/run-soak.sh 18443 4 --json-out ...
#       # h3 那一档要对端是 aioquic：用 ASYN_SOAK_PYTHON 指到有 aioquic 的那个解释器
#
# 录满一份基线要跑的五档（与 baseline-linux.json 的键一一对应）：
#   run-soak.sh                                  --json-out build/soak-r1.json        # http1-keepalive + http1-churn
#   ASYN_SOAK_STAGE=h2c run-soak.sh ... --pipeline 1  --json-out build/h2c-p1-r1.json # http2-h2c-pipeline1
#   ASYN_SOAK_STAGE=h2c run-soak.sh ... --pipeline 32 --json-out build/h2c-p32-r1.json
#   ASYN_SOAK_STAGE=h3  run-soak.sh ... --json-out build/h3-r1.json                   # http3-load
#   每档各跑三次，再按中位数与离散范围写进基线；然后
#   python3 benchmarks/check-baseline.py build/*.json --baseline benchmarks/baseline-linux.json
set -uo pipefail

# 仓库根取自脚本自己的位置，不靠 git rev-parse：被从别的树调用时会拿错根
repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="${1:-18080}"
threads="${2:-4}"
shift 1 2>/dev/null || true
shift 1 2>/dev/null || true
stage="${ASYN_SOAK_STAGE:-h1}"
build="${ASYN_SOAK_BUILD:-debug}"
python_binary="${ASYN_SOAK_PYTHON:-python3}"
certificate="${ASYN_SOAK_CERT:-$repository_root/tests/Core/fixtures/test_cert.pem}"
private_key="${ASYN_SOAK_KEY:-$repository_root/tests/Core/fixtures/test_key.pem}"

case "${stage}" in
    h1|h2c|h3) ;;
    *) echo "未知的 ASYN_SOAK_STAGE=${stage}（可选 h1、h2c、h3）" >&2; exit 2 ;;
esac

server_binary="${repository_root}/build/${build}/samples/echo_server"
if [ ! -x "${server_binary}" ]; then
    echo "找不到可执行体 ${server_binary}（先 cmake --build build/${build} --target echo_server）" >&2
    exit 1
fi

# 服务端的输出必须落到文件：它挂了的话，没有这行日志就只能猜是配置错还是崩了
server_log="${TMPDIR:-/tmp}/asyn-soak-server-${port}.log"
server_args=(--port "${port}" --threads "${threads}")
case "${stage}" in
    h2c) server_args+=(--h2c) ;;
    h3)
        for path in "${certificate}" "${private_key}"; do
            if [ ! -f "${path}" ]; then
                echo "h3 那一档要证书，找不到 ${path}（用 ASYN_SOAK_CERT/ASYN_SOAK_KEY 指路）" >&2
                exit 2
            fi
        done
        server_args+=(--https --h3 --compress --cert "${certificate}" --key "${private_key}")
        ;;
esac

echo "启动 ${server_binary} ${server_args[*]}，日志 ${server_log}"
"${server_binary}" "${server_args[@]}" >"${server_log}" 2>&1 &
server_pid=$!

cleanup() {
    kill "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
}
trap cleanup EXIT

# 就绪判据分两档：TCP 那一档能 connect 出来才算起来，别拿固定 sleep 当就绪。
# UDP（h3）连不上也看不出区别，只能让它自己用「第一条请求是否答上」当判据，失败时日志尾巴在这。
if [ "${stage}" != "h3" ]; then
    deadline=$((SECONDS + 30))
    while ! (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; do
        if [ "${SECONDS}" -ge "${deadline}" ]; then
            echo "${port} 端口 30 秒内没监听起来，服务端日志：" >&2
            tail -20 "${server_log}" >&2
            exit 1
        fi
        if ! kill -0 "${server_pid}" 2>/dev/null; then
            echo "服务端进程已经退出，${port} 不会有人监听。日志：" >&2
            tail -20 "${server_log}" >&2
            exit 1
        fi
        sleep 0.3
    done
else
    if ! kill -0 "${server_pid}" 2>/dev/null; then
        echo "服务端没能起来（UDP 探不出来，只能先看进程在不在）。日志：" >&2
        tail -20 "${server_log}" >&2
        exit 1
    fi
    sleep 3
fi

case "${stage}" in
    h1) soak_script="${repository_root}/benchmarks/soak.py" ;;
    h2c) soak_script="${repository_root}/benchmarks/soak_h2c.py" ;;
    h3) soak_script="${repository_root}/benchmarks/h3_soak.py" ;;
esac

# soak_h2c.py 不收 --pid（它自己不做资源采样），传过去会被 argparse 判成未知参数
soak_args=(--port "${port}")
if [ "${stage}" != "h2c" ]; then
    soak_args+=(--pid "${server_pid}")
fi

"${python_binary}" "${soak_script}" "${soak_args[@]}" "$@"
soak_rc=$?

if [ "${soak_rc}" != "0" ]; then
    echo "压测退出码 ${soak_rc}，服务端日志尾巴：" >&2
    tail -20 "${server_log}" >&2
fi

# 成功与否按压测脚本自己的判定算：服务端是被工具，不是被测对象
exit "${soak_rc}"
