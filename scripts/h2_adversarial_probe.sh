#!/usr/bin/env bash
# HTTP/2 明文（h2c）对手探针：用 curl 自带的 nghttp2 这套独立实现去打本框架的 echo_server，
# 专走「客户端不按常理收场」那几条路——读到一半放弃（RST_STREAM）、超大的头部、HEAD、
# Expect: 100-continue、一条连接上并发多路复用。
#
# 选独立实现而不是自写客户端的理由与 h3 探针一样：两边对协议的理解不一致才会在这里露出来，
# 自写探针只会重复本框架自己的误会。h3 那侧就是这样查出一条「客户端每条连接取消一次请求
# 就能把连接打掉」的问题，这条探针是给 h2 的同等覆盖。
#
# 探针自己要能被证伪：第 4 项（超大头部按 431）与第 8 项（合法放弃不记成坏请求）都读 /metrics 或
# 读服务端给的状态码，而不是只看 curl 的退出码——只看 curl 会测到空气（实测：正文一秒就写完的
# /sse 用 --max-time 去放弃，服务端一条取消都不会记）。取数需要服务端带 --metrics 启动。
# 差值判据成立的前提是「这条计数是进程口径」：同一端口上多个监听器各持一份采集端时，一次抓取
# 只命中其中一台，读数能在两次抓取之间变小。第 6 项前先做一道相邻两取的自证，把这种情况报出来
# 而不是拿它当「服务端记坏了请求」（echo_server 从 2026-09-23 起让全部监听器共用一份采集端）。
#
# 用法：h2_adversarial_probe.sh <port>
# 退出码 0 = 全部核对通过；非 0 时把每个失败项打到 stderr。
set -u

PORT="${1:-18080}"
BASE="http://127.0.0.1:${PORT}"
CURL=(curl -s --http2-prior-knowledge --max-time 8 -o /dev/null)
FAILED=0

fail()
{
    echo "FAIL: $*" >&2
    FAILED=1
}

status_of()
{
    "${CURL[@]}" -w '%{http_code}' "$@"
}

# 取一项指标的当前值。/metrics 本身也走 h2c，因此它自己会进 requests_total——
# 只有 stream_cancelled 这一类不受取数影响，探针就只拿它当证据
metric_of()
{
    curl -s --http2-prior-knowledge --max-time 8 "${BASE}/metrics" 2>/dev/null \
        | awk -v name="asyn_http_$1" '$1 == name { print $2; exit }'
}

# 1) 常规 GET：h2c 这条协商路径本身要成立
status=$(status_of "${BASE}/json")
[ "$status" = "200" ] || fail "常规 GET /json 实得 ${status}"

# 2) HEAD：状态与头部照常，但正文必须零字节
head_readout=$(curl -s -I --http2-prior-knowledge --max-time 8 -o /dev/null -w '%{http_code}|%{size_download}' "${BASE}/big")
[ "$head_readout" = "200|0" ] || fail "HEAD /big 应为 200 且零字节正文，实得 ${head_readout}"

# 3) 方法不匹配：/json 只挂 GET，POST 要按 405 答复而不是 hang
status=$(status_of -X POST --data-binary 'a body' "${BASE}/json")
[ "$status" = "405" ] || fail "POST /json 应按 405 收口，实得 ${status}"

# 4) 超大头部：单条头值远超本端上限。必须按 431 答复并且**只作废这一条流**——实得 000 说明连接
#    被这条请求带走了（那是本端把越限当成压缩上下文错误的旧口径）。上限之内的单条字段（8000 字节）
#    要照常拿到 200，否则这条检查就成了「什么都拒」也算通过。
#    注：单条 70 KB 的字段到不了服务端——curl 在本地就拒着发（实测退出码 55），那种形状量的是 curl
huge=$(head -c 20000 /dev/zero | tr '\0' 'a')
status=$(status_of -H "x-huge: ${huge}" "${BASE}/json")
[ "$status" = "431" ] || fail "超大头部应按 431 只作废这一条流，实得 ${status}（000 表示整条连接被带走）"
status=$(status_of -H "x-mid: $(head -c 8000 /dev/zero | tr '\0' 'b')" "${BASE}/json")
[ "$status" = "200" ] || fail "上限之内的 8000 字节头值应正常应答，实得 ${status}"

# 5) Expect: 100-continue：本框架的 h1 明确不发 100，那 h2 侧也必须直接答复文，
#    不能让客户端等着那个永远不会来的中间响应直到超时
status=$(status_of -X POST -H 'Expect: 100-continue' --data-binary 'continued body' "${BASE}/json")
[ "$status" = "405" ] || fail "Expect: 100-continue 的 POST 没有直接拿到 405，实得 ${status}"

# 6) 一批「读到一半放弃」的流：正文选 /big（256 KiB）并把客户端读速压到每秒 32 字节，一秒内只可能
#    读到几十字节，放弃一定落在响应还没被读完的时候。
#    这批不核对 asyn_http_http2_stream_cancelled_total：实测那条计数在 curl 这条路上一动不动——
#    本端把 2xx 记在「响应排入待发字节」那一刻，而那个计数的口径是「本端因此未发响应」，两条永远对不上。
#    价值在于另一件事：客户端在响应写了一半时把连接抽走，这条路径在 ASan 下不能报任何东西。
#    「对端在响应发出前取消这条流」由 tests/Net/Http2 的会话用例覆盖（那里能精确控制 RST 的时机）
beforeAborts=$(metric_of bad_requests_total)
[ -n "$beforeAborts" ] || fail "读不到 asyn_http_bad_requests_total，服务端没带 --metrics 启动？"
# 相邻两次抓取必须不减：这条计数是进程级单调量，读数变小只有两种解释——端口上坐着不止一个
# 监听进程（上一轮没杀干净），或者这些监听器没共用一份采集端（各自报自己那 1/N）。
# 两种情况都让下面的差值判据失去意义，所以先自证取数通道，再谈「多出了几条」
beforeAbortsAgain=$(metric_of bad_requests_total)
[ -n "$beforeAbortsAgain" ] || fail "第二次取 asyn_http_bad_requests_total 就取空了：服务端中途停了？"
if [ "$beforeAbortsAgain" -lt "$beforeAborts" ]; then
    fail "相邻两次抓取里 ${beforeAborts} → ${beforeAbortsAgain}：计数不是进程口径，端口上有多余的监听进程或采集端没共用"
    beforeAborts="$beforeAbortsAgain"
fi
for attempt in $(seq 1 8); do
    curl -s --http2-prior-knowledge --max-time 1 --limit-rate 32 -o /dev/null "${BASE}/big" >/dev/null 2>&1 &
done
wait

# 7) 放弃之后服务必须照常：会话级与连接级的状态没被拖坏
status=$(status_of "${BASE}/json")
[ "$status" = "200" ] || fail "一批异常收场之后 GET /json 实得 ${status}"

# 8) 一批「读到一半放弃」与超时不许被记成解析失败或协议错误：这类客户端行为完全合法，
#    记坏了请求数会让运维看到的是「对端在发坏请求」，而真相是本端把发送背压当成了故障
afterAborts=$(metric_of bad_requests_total)
# 取空要当场判失败，不能悄悄跳过：静默跳过等于这条检查在服务器上什么都没核对
[ -n "$afterAborts" ] || fail "八次放弃之后取不到 asyn_http_bad_requests_total，无法核对坏请求计数"
badFromAborts=$((afterAborts - beforeAborts))
[ "$badFromAborts" = "0" ] || fail "八次读到一半放弃多出 ${badFromAborts} 条坏请求记录（放弃前 ${beforeAborts}，放弃后 ${afterAborts}）"

# 9) 一条连接上真并发多路复用：同一个 curl 进程带多个 URL 加 -Z 才会把它们并发复用到同一条连接。
#    八个进程各带一个 URL 是八条连接，量的就不是多路复用了。
#    -o 与 URL 按位置配对，一个 -o /dev/null 只管第一条——其余正文会直接打到标准输出上
parallel=$(curl -s -Z --http2-prior-knowledge --max-time 8 -w '%{http_code}\n' \
    -o /dev/null -o /dev/null -o /dev/null -o /dev/null \
    -o /dev/null -o /dev/null -o /dev/null -o /dev/null \
    "${BASE}/json" "${BASE}/json" "${BASE}/json" "${BASE}/json" \
    "${BASE}/json" "${BASE}/json" "${BASE}/json" "${BASE}/json" | sort -u | tr -d '\n')
[ "$parallel" = "200" ] || fail "一条连接上并发八条流的结果码不是一色的 200：${parallel}"

# 10) 再来一轮「放弃 + 正常」交错，确认反复放弃不会把连接级额度一点点吃掉。
#     每轮先放弃一条 /big 再取一条 /json：额度没还回来时先受害的是后面这条正常请求
for attempt in $(seq 1 6); do
    curl -s --http2-prior-knowledge --max-time 1 --limit-rate 32 -o /dev/null "${BASE}/big" >/dev/null 2>&1
    status=$(status_of "${BASE}/json")
    [ "$status" = "200" ] || fail "第 ${attempt} 轮放弃之后的正常请求实得 ${status}"
done

[ "$FAILED" = "0" ] && echo "HTTP/2 对手探针全部通过"
exit "$FAILED"
