#!/usr/bin/env bash
# QUIC 出站方向的跨实现验收：aioquic（独立实现）起服务端并回显，本框架的探针当客户端。
#
# 与 scripts/quic_cross_check.sh 的方向相反：那份是「外部客户端打我们的服务端」，这份是
# 「我们的客户端打外部服务端」。角色化传输核心之后必须两边都验，否则两型实现共享的误解
# （密钥方向、该不该发 original_destination_connection_id、握手确认的时刻）验不出来。
#
# 判据取两边的说法：aioquic 侧打印它解到了什么，探针侧打印它做了什么，任何一侧缺行即失败。
# **不要只看退出码**——脚本没跑起来与跑起来但判据不成立，都可能是非 0 或 0。
#
# 用法：
#   scripts/quic_outbound_cross_check.sh [构建目录] [带 aioquic 的 python]
#   scripts/quic_outbound_cross_check.sh build-asan /root/h3venv/bin/python
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root" || exit 1

buildDir="${1:-build/release}"
python="${2:-python3}"
# 证书路径保持**相对**形式：脚本已经 cd 到仓库根。给绝对路径的话，Git Bash 会把
# /g/Codes/... 这种 MSYS 形状递给 Windows 版 python，对方打不开就崩，而报出来的是「对端没起来」
certificate="tests/Core/fixtures/test_localhost_cert.pem"
privateKey="tests/Core/fixtures/test_localhost_key.pem"
# 对端答回来的正文与 .py 里的 SERVED_BODY 必须一致，长度也写进判据里
servedBody="aioquic-h3-served"
logDir="$(mktemp -d)"
serverPid=""
failures=0

# 探针的落点随平台与生成器而异，逐个试；全找不到就报出来
probe=""
for candidate in \
        "$buildDir/tests/Tools/quic_probe_client" \
        "$buildDir/tests/Tools/quic_probe_client.exe" \
        "$buildDir/tests/tools/quic_probe_client"; do
    if [ -x "$candidate" ] || [ -f "$candidate" ]; then
        probe="$candidate"
        break
    fi
done

# 缺东西就明确报出来，别把「没跑」混成「跑过了」
if [ -z "$probe" ]; then
    echo "缺少探针可执行体：$buildDir/tests/Tools/quic_probe_client"
    exit 2
fi
if [ ! -f "$certificate" ] || [ ! -f "$privateKey" ]; then
    echo "缺少证书夹具：$certificate / $privateKey"
    exit 2
fi
if ! "$python" -c 'import aioquic' >/dev/null 2>&1; then
    echo "第二个参数指向的 python 里没有 aioquic：$python"
    exit 2
fi

findFreePort() {
    # 单行 -c 而不是 heredoc：heredoc 形式在 Git Bash 下会把附带的实参当成脚本内容，端口取回来是空的，
    # 于是服务端收到 `int("")` 直接崩——报出来的是「对端没起来」，看不出根因在这一步
    "$python" -c 'import socket; sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.bind(("127.0.0.1", 0)); print(sock.getsockname()[1]); sock.close()' 2>/dev/null
}

stopServer() {
    if [ -n "$serverPid" ] && kill -0 "$serverPid" 2>/dev/null; then
        kill "$serverPid" 2>/dev/null
        wait "$serverPid" 2>/dev/null
    fi
    serverPid=""
}

# expectLine <文件> <正则> <判据说明>
expectLine() {
    local file="$1" pattern="$2" label="$3"
    if grep -aqE "$pattern" "$file"; then
        echo "PASS $label"
    else
        echo "FAIL $label（没在 $(basename "$file") 里看到 /$pattern/）"
        failures=$((failures + 1))
    fi
}

# runScenario <名字> <服务端 alpn> <探针 --alpn 值> <期望握手成?>
runScenario() {
    local name="$1" serverAlpn="$2" clientAlpn="$3" expectHandshake="$4"
    local serverLog="$logDir/$name.server.log"
    local clientLog="$logDir/$name.client.log"
    local port
    port="$(findFreePort)"
    if [ -z "$port" ]; then
        echo "FAIL $name：拿不到空闲端口"
        failures=$((failures + 1))
        return
    fi

    "$python" scripts/quic_outbound_cross_check.py 127.0.0.1 "$port" "$certificate" "$privateKey" "$serverAlpn" 12 \
        >"$serverLog" 2>&1 &
    serverPid=$!

    local isListening=0
    for _ in $(seq 1 100); do
        if grep -aq '^LISTENING' "$serverLog" 2>/dev/null; then
            isListening=1
            break
        fi
        sleep 0.1
    done
    if [ "$isListening" -ne 1 ]; then
        echo "FAIL $name：对端服务端没起来"
        sed 's/^/    服务端: /' "$serverLog" 2>/dev/null | head -5
        failures=$((failures + 1))
        stopServer
        return
    fi

    # MSYS2_ARG_CONV_EXCL：Git Bash 会把 `--path /probe` 里那个以 / 开头的取值换成一个真实存在的
    # Windows 路径（实测换成 C:/Users/.../probe），本端的 :path 校验会把它判成非法——
    # 看着像实现坏了，其实是命令行参数被换算过（本仓库踩过两次同型坑）
    MSYS2_ARG_CONV_EXCL='*' "$probe" --port "$port" --host localhost --ca "$certificate" --alpn "$clientAlpn" \
        --path /probe --handshake-timeout 3000 --wait-timeout 6000 >"$clientLog" 2>&1
    local clientExit=$?

    local servedBytes=${#servedBody}
    if [ "$expectHandshake" = "ok" ]; then
        expectLine "$serverLog" '^HANDSHAKE alpn='"$serverAlpn"'$' "$name：对端解出了握手并协商到 $serverAlpn"
        expectLine "$serverLog" "^REQUEST [0-9]+ GET /probe\$" "$name：对端把我们的请求解成了 GET /probe"
        expectLine "$serverLog" "^ANSWERED [0-9]+ $servedBytes\$" "$name：对端答出了 $servedBytes 字节正文"
        expectLine "$clientLog" '^CONNECTED '"$clientAlpn"'$' "$name：本端握手完成且 ALPN 一致"
        expectLine "$clientLog" '^HEADERS$' "$name：本端开出了控制流与两条 QPACK 流"
        expectLine "$clientLog" "^RESPONSE 200 $servedBytes\$" "$name：本端收到 200 与 $servedBytes 字节正文"
        expectLine "$clientLog" '^BODY_MATCH$' "$name：正文与对端所答逐字相同"
        expectLine "$clientLog" '^RESULT ok$' "$name：探针自评为整趟走通"
        if [ "$clientExit" -ne 0 ]; then
            echo "FAIL $name：事件行都齐了但探针退出码是 $clientExit"
            failures=$((failures + 1))
        fi
    else
        # ALPN 谈不拢时握手根本完不成：对端不会报 HandshakeCompleted，而是直接 terminate。
        # 于是这一支要看的是「对端确实收到了我们的报文并以告警收口」+「本端也报失败」，
        # 缺一条就是假的对称（比如本端超时才算失败、对面其实握上了）
        expectLine "$serverLog" '^INBOUND ' "$name：对端收到了我们发出的第一个报文"
        if grep -aq '^HANDSHAKE alpn=' "$serverLog"; then
            echo "FAIL $name：ALPN 不一致，对端却报握手已完成"
            failures=$((failures + 1))
        else
            echo "PASS $name：对端没有报握手完成"
        fi
        expectLine "$clientLog" '^RESULT failed$' "$name：探针自评为没走通"
        if [ "$clientExit" -eq 0 ]; then
            echo "FAIL $name：ALPN 被拒却以退出码 0 收场"
            failures=$((failures + 1))
        fi
    fi

    stopServer
}

runScenario alpn-h3 h3 h3 ok
runScenario alpn-mismatch h3 h9 refuse

echo "----"
echo "FAILURES=$failures"
rm -rf "$logDir"
[ "$failures" -eq 0 ] || exit 1
echo "QUIC_OUTBOUND_CROSS_CHECK_OK"
