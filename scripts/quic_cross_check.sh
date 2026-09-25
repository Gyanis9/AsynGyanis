#!/usr/bin/env bash
# QUIC 跨实现验收：起本框架的服务端夹具，逐场景让 aioquic（独立实现）当对端。
#
# 这份脚本是「裁判」的接线板：判据在 scripts/quic_cross_check.py 里（由 aioquic 的解析器给出），
# 服务端侧的事件由 tests/Tools/QuicProbeServer.cpp 打成 stdout 行，这里负责起停、取端口、
# 核对两边的说法是否一致。任一条不成立就退出码非 0。
#
# 用法：
#   scripts/quic_cross_check.sh [构建目录] [带 aioquic 的 python]
#   scripts/quic_cross_check.sh build/release /root/h3venv/bin/python
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root" || exit 1

buildDir="${1:-build/release}"
python="${2:-python3}"
cert="$root/tests/Core/fixtures/test_cert.pem"
key="$root/tests/Core/fixtures/test_key.pem"
logDir="$(mktemp -d)"
workDir="$(mktemp -d)"
failures=0
serverPid=""

# 构建目录里没这两份东西就没法验：明确报出来，别把「没跑」混成「跑过了」
if [ ! -d "$buildDir" ]; then
    echo "构建目录不存在：$buildDir"
    exit 2
fi
if [ ! -f "$cert" ] || [ ! -f "$key" ]; then
    echo "缺少证书夹具：$cert / $key"
    exit 2
fi

findServer()
{
    local candidate
    for candidate in \
        "$buildDir/tests/Tools/quic_probe_server" \
        "$buildDir/tests/Tools/quic_probe_server.exe" \
        "$buildDir/tests/tools/quic_probe_server"; do
        if [ -x "$candidate" ] || [ -f "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

serverBinary="$(findServer)" || {
    echo "找不到 quic_probe_server：先构建它（cmake --build $buildDir --target quic_probe_server）"
    exit 2
}

stopServer()
{
    if [ -n "$serverPid" ] && kill -0 "$serverPid" 2>/dev/null; then
        kill -INT "$serverPid" 2>/dev/null
        wait "$serverPid" 2>/dev/null
    fi
    serverPid=""
}

trap stopServer EXIT

# startServer <日志名> <参数…>：起服务、等 READY、把实际端口打到 stdout
startServer()
{
    local label="$1"
    shift
    "$serverBinary" --cert "$cert" --key "$key" "$@" > "$logDir/$label.log" 2>&1 &
    serverPid=$!
    for _ in $(seq 1 100); do
        if grep -q '^READY$' "$logDir/$label.log" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$serverPid" 2>/dev/null; then
            echo "服务端起不来（$label）："
            tail -5 "$logDir/$label.log"
            return 1
        fi
        sleep 0.1
    done
    if ! grep -q '^READY$' "$logDir/$label.log" 2>/dev/null; then
        echo "服务端 10 秒内没宣告就绪（$label）"
        return 1
    fi
    awk '/^PORT / {print $2; exit}' "$logDir/$label.log"
}

# runCase <用例名> <日志名> <python 场景参数…>：探针退出码即判据
runCase()
{
    local caseName="$1"
    shift 2          # 日志名只是给本地留档用，不进探针的参数
    if "$python" "$root/scripts/quic_cross_check.py" "$@" > "$logDir/$caseName.out" 2>&1; then
        echo "  PASS $caseName"
    else
        echo "  FAIL $caseName"
        tail -3 "$logDir/$caseName.out"
        failures=$((failures + 1))
    fi
}

# expectLog <用例名> <日志名> <正则>：服务端自己的事件行也要对上（客户端单侧看不见的那部分）
expectLog()
{
    local caseName="$1" label="$2" pattern="$3"
    if grep -qE "$pattern" "$logDir/$label.log" 2>/dev/null; then
        echo "  PASS $caseName（服务端事件已核对）"
    else
        echo "  FAIL $caseName：日志里找不到 $pattern"
        tail -6 "$logDir/$label.log"
        failures=$((failures + 1))
    fi
}

head -c 48 /dev/urandom > "$workDir/ticket.key" || { echo "造不出票据密钥文件"; exit 2; }

echo "== 场景一：单实例基本行为（握手、回显、垃圾包、ALPN、流收尾、drain、限额、流控不饿死）"
port="$(startServer basic --abort-on abort --abort-code 0x010b --large-reply-on block)" || exit 2
runCase handshake-echo basic handshake-echo "$port"
expectLog handshake-echo basic 'ECHOED [0-9]+ 9'
stopServer

port="$(startServer garbage --idle-timeout 30)" || exit 2
runCase garbage-tolerant garbage garbage-tolerant "$port"
stopServer

port="$(startServer alpn)" || exit 2
runCase alpn-refusal alpn alpn-refusal "$port"
stopServer

port="$(startServer abort --abort-on abort --abort-code 0x010b)" || exit 2
runCase stream-abort abort stream-abort "$port" 0x010B
expectLog stream-abort abort '^ABORTED [0-9]+ 267$'
stopServer

port="$(startServer drain --drain-immediately)" || exit 2
runCase drain-refuses-new drain drain-refuses-new "$port"
stopServer

port="$(startServer close --drain-after-request 0)" || exit 2
runCase close-on-drain close close-on-drain "$port"
expectLog close-on-drain drain DRAINING
stopServer

port="$(startServer limit --per-ip-limit 1)" || exit 2
runCase per-ip-limit limit per-ip-limit "$port"
stopServer

port="$(startServer starve --large-reply-on block --large-reply-bytes 4194304)" || exit 2
runCase no-starve starve no-starve "$port"
stopServer

port="$(startServer idle --idle-timeout 1)" || exit 2
runCase idle-reap idle idle-reap "$port"
stopServer

port="$(startServer abandon --idle-timeout 2)" || exit 2
runCase handshake-abandon abandon handshake-abandon "$port"
# 半开连接要被服务端自己收掉：先看到 1 条在线，再看到归零
expectLog handshake-abandon-reap abandon '^CONNECTIONS 1$'
expectLog handshake-abandon-reap abandon '^CONNECTIONS 0$'
stopServer

echo "== 场景二：跨实例会话恢复（对照装与不装同一份票据密钥）"
portA="$(startServer resume-a --ticket-key "$workDir/ticket.key")" || exit 2
portB="$(startServer resume-b --ticket-key "$workDir/ticket.key")" || exit 2
runCase resume resume-a resume "$portA" "$portB"
stopServer

portA="$(startServer miss-a --ticket-key "$workDir/ticket.key")" || exit 2
portB="$(startServer miss-b)" || exit 2
runCase resume-miss miss-a resume-miss "$portA" "$portB"
stopServer

echo "== 汇总"
if [ "$failures" -eq 0 ]; then
    echo "全部场景通过（$("$python" -c 'import aioquic, sys; print(getattr(aioquic, "__version__", "unknown"))' 2>/dev/null || echo "?")）"
    exit 0
fi
echo "失败 $failures 条，日志在 $logDir"
exit 1
