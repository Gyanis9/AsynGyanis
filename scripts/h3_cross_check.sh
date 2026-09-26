#!/usr/bin/env bash
# HTTP/3 的对外一致性验收：起一份 echo_server --h3，再用 aioquic（独立实现）按场景逐条打它。
#
# 为什么需要这份脚本：h3 会话用例的对端是测试自己按 RFC 9114/9204 排字节的（第三方实现已移出
# 构建），它对「字节合不合规范」只算自证。跨实现的判定必须在**进程外**做——aioquic 不认识本仓的
# 代码，也不会跟着本仓一起被改软：头块编错、HEAD 漏了正文、流式响应并成一趟、该忽略的头被吞了，
# 都只在这里才会变红。
#
# 用法：bash scripts/h3_cross_check.sh [构建目录] [python 可执行体]
# 端口可用 ASYN_H3_CROSS_PORT 覆盖；ASYN_H3_CROSS_LOG 指定服务端日志落点（CI 要取它当工件）；
# ASYN_H3_TRACE=1 把每条场景的收发事件打出来。
set -uo pipefail

build_dir="${1:-build/release}"
python_binary="${2:-python3}"
server_binary="$build_dir/samples/echo_server"
port="${ASYN_H3_CROSS_PORT:-18493}"
# 仓库内的自签夹具：探针以 CERT_NONE 连接，CN 与 SAN 不参与校验
certificate="tests/Core/fixtures/test_cert.pem"
private_key="tests/Core/fixtures/test_key.pem"
server_log="${ASYN_H3_CROSS_LOG:-$(mktemp)}"
# 一次性的小静态站点：让 h3 的静态正文与两个验证器也接受独立实现的核对。会话层用例的对端是
# 测试自己排字节的替身，它看不见「content-length 与文件里的字节数是否同源」这类跨实现的事实
static_dir="$(mktemp -d)"
static_body="hello-h3-static-body"
printf '%s' "$static_body" > "$static_dir/greeting.txt"
static_bytes=$(wc -c < "$static_dir/greeting.txt" | tr -d ' ')

if [[ ! -x "$server_binary" ]]; then
    echo "找不到可执行体 $server_binary（先 cmake --build $build_dir --target echo_server）" >&2
    exit 1
fi
if ! command -v "$python_binary" >/dev/null 2>&1; then
    echo "找不到 python 可执行体：$python_binary" >&2
    exit 1
fi

"$server_binary" --port "$port" --threads 1 --https --h3 --compress --cert "$certificate" --key "$private_key" \
    --static "$static_dir" >"$server_log" 2>&1 &
server_pid=$!

cleanup() {
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    rm -rf "$static_dir" 2>/dev/null || true
}
trap cleanup EXIT

# UDP 端口没法用 connect 探测，就拿探针本身当就绪判据：它连不上时会非 0 退出
ready_attempt=0
until "$python_binary" scripts/h3_acceptance.py 127.0.0.1 "$port" /bench 200 >/dev/null 2>&1; do
    ready_attempt=$((ready_attempt + 1))
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "服务端在就绪之前退了，日志：" >&2
        tail -n 40 "$server_log" >&2
        exit 1
    fi
    if [[ $ready_attempt -ge 20 ]]; then
        echo "等了 20 秒 h3 端口仍不应答，日志：" >&2
        tail -n 40 "$server_log" >&2
        exit 1
    fi
    sleep 1
done

failures=0

# runScenario <标签> <探针参数...>：一条不过就把标签记下来，最后统一报（中途退出会漏掉后面的场景）
runScenario() {
    local label="$1"
    shift
    printf '%-22s ' "$label"
    if "$python_binary" scripts/h3_acceptance.py 127.0.0.1 "$port" "$@"; then
        printf 'PASS\n'
    else
        printf 'FAIL\n'
        failures=$((failures + 1))
    fi
}

# 普通 GET：状态码、正文，以及由独立实现解出来的响应头。content-length 与正文长度必须同源，
# date 由会话补齐——这两条都是「本端自解自」看不见、换一个实现才会露出来的判据
runScenario get-bench /bench 200 OK
runScenario get-headers /bench 200 --expect-header content-type=text/plain \
    --expect-header content-length=2 --expect-header date
# HEAD：RFC 9110 §9.3.2 要它给出 GET 会发出的那份头部，但线上一个正文字节都不许有。
# content-length=2 与上面 GET 的正文长度同源——这一条正是 aioquic 自己看不见的那处（它不记方法）
runScenario head-bench /bench --head --expect-header content-length=2
# 流式响应：正文分趟到齐（并成一趟是错的，压根没分趟也是错的），且长度此刻还不知道
runScenario stream-sse /sse --stream 200 "data: two"
# 大正文：跨多个 DATA 帧，各段要按序拼回原样
runScenario big-body /big 200
# 压缩链路：声明了 gzip 就必须真压上（探针解开后核对片段），且 vary 带上 accept-encoding
runScenario gzip-big /big --accept-encoding gzip 200
# 扩展 CONNECT（RFC 9220）隧道：2xx 建立 + 两条帧的回显逐字节对上
runScenario websocket-tunnel /ws --websocket hello-h3
# 静态文件：正文按字节取回、content-type 按扩展名推出、两个验证器（etag 与 last-modified）都在场。
# 长度用文件实际字节数，而不是写死的字面串——「HEAD 与 GET 的 content-length 同源」这条判据才不是自证
runScenario static-get /greeting.txt 200 "$static_body" \
    --expect-header content-type --expect-header "content-length=$static_bytes" \
    --expect-header etag --expect-header last-modified
# 同一条静态资源的 HEAD：要给出 GET 会发的那份头部（含同一个长度），线上一个正文字节都不许有
runScenario static-head /greeting.txt --head --expect-header "content-length=$static_bytes" --expect-header etag
# 目录里没有的名字：兜底路由要把请求让给 404，而不是回一份空正文当作命中
runScenario static-missing /no-such-file.txt 404

if [[ $failures -ne 0 ]]; then
    echo "HTTP/3 跨实现验收失败 $failures 条" >&2
    echo "服务端日志末尾：" >&2
    tail -n 20 "$server_log" >&2
    exit 1
fi

echo "HTTP/3 跨实现验收全部通过（11 条场景）"
