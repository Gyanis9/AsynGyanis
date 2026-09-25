#!/usr/bin/env bash
set -euo pipefail

# WebSocket 一致性验收：用 Autobahn|Testsuite（另一家独立实现，517 条用例）打一个正在跑的
# echo_server 的 WebSocket 路由。
#
# 为什么要引外部裁判：自家探针与实现同源，查不出「两边都错在同一种理解上」。Autobahn 每条用例
# 给的是「期望的事件序列」，本机这一轮就是这样查出两件事的——
#   * 1.2.x / 9.2.x / 12.2.x 等三十六上：回显不分类型一律发文本帧，本端编码器随即按 1007 拒发
#     （文本负载必须是合法 UTF-8，RFC 6455 §5.6），连接断在对端等回显的时候；
#   * 9.1.4 / 9.3.9：单帧上限(1 MiB)比消息上限(8 MiB)还小，于是「拆片收得下、整片发来回 1009」。
#
# 用法：先把服务端起起来（回显要按原类型回帧），再跑本脚本
#   build/release/samples/echo_server --port 18081 --threads 2 --metrics &
#   scripts/websocket_autobahn.sh 18081
#
# 镜像：默认用官方 crossbario/autobahn-testsuite。取不到 Docker Hub 的环境（本机就是）可以用
# 自备镜像：按官方 docker/Dockerfile 里的 py2.7 依赖装一份，再 AUTOBAHN_IMAGE=... 指过来。
# 两个镜像的调用面相同（/usr/local/bin/wstest -m fuzzingclient -s <spec>）。
#
# 可选项：
#   AUTOBAHN_IMAGE      镜像名（默认 crossbario/autobahn-testsuite:latest）
#   AUTOBAHN_CASES      JSON 数组字面量，默认 '["*"]'；只跑几条快速回归时就填具体号
#   AUTOBAHN_EXCLUDE    逗号分隔的用例号，默认排掉超容量的那两条（见下）
#   AUTOBAHN_REPORT_DIR 报告落点，默认仓库外的临时目录
#
# 判据：报告里 behavior=FAILED 的条数必须为 0。默认排除 9.1.6 与 9.2.6——那两条发 16 MiB 的
# 消息，超本端单条消息 8 MiB 的上限，按 RFC 6455 §7.1.6 回 1009 收口属规范许可的拒绝，
# Autobahn 却仍按「没收到回显」记 FAILED。要连它们一起看，AUTOBAHN_EXCLUDE= 传空即可。

port="${1:-18081}"
ws_path="${2:-/ws}"
image="${AUTOBAHN_IMAGE:-crossbario/autobahn-testsuite:latest}"
cases="${AUTOBAHN_CASES:-[\"*\"]}"
exclude="${AUTOBAHN_EXCLUDE-9.1.6,9.2.6}"

if ! curl -s -o /dev/null --max-time 3 "http://127.0.0.1:${port}/json"; then
    echo "没人在 ${port} 上应答：先把 echo_server 起起来（--port ${port} --metrics），再来跑本脚本" >&2
    exit 2
fi

work_dir="$(mktemp -d)"
report_dir="${AUTOBAHN_REPORT_DIR:-${TMPDIR:-/tmp}/autobahn-ws-reports}"
trap 'rm -rf "${work_dir}"' EXIT
mkdir -p "${report_dir}"
rm -rf "${report_dir:?}"/*

# exclude-cases 要的是 JSON 数组：把 9.1.6 这样的号换成 "9.1.6"
exclude_json=""
for case_id in ${exclude//,/ }; do
    [ -n "${case_id}" ] || continue
    exclude_json="${exclude_json:+${exclude_json}, }\"${case_id}\""
done

cat > "${work_dir}/fuzzingclient.json" <<EOF
{
   "outdir": "/reports",
   "servers": [
      {"url": "ws://host.docker.internal:${port}${ws_path}", "agent": "AsynGyanis echo_server"}
   ],
   "cases": ${cases},
   "exclude-cases": [${exclude_json}],
   "exclude-agent-cases": {}
}
EOF

echo "Autobahn|Testsuite 打 ws://host.docker.internal:${port}${ws_path}（镜像 ${image}，排除 ${exclude:-无}）"
# 挂载源要写成 Docker 看得懂的形式：Git Bash 里 pwd -W 给出 C:/… 这样的 Windows 路径，
# Linux 上没有这个选项，退回普通 pwd
mount_source() {
    (cd "$1" && pwd -W 2>/dev/null) || (cd "$1" && pwd)
}

# MSYS_NO_PATHCONV：Git Bash 会把以 / 开头的参数（这里是 --entrypoint 的容器内路径）换算成
# Windows 路径，换算完容器里就没有这个文件了。挂载源已用 pwd -W 写成 Windows 形式，无需再换算
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' docker run --rm -i --entrypoint /usr/local/bin/wstest \
    -v "$(mount_source "${work_dir}")/fuzzingclient.json:/fuzzingclient.json:ro" \
    -v "$(mount_source "${report_dir}"):/reports" \
    "${image}" -m fuzzingclient -s /fuzzingclient.json > "${work_dir}/wstest.log" 2>&1 || true

failed_count="$(grep -l '"behavior": "FAILED"' "${report_dir}"/*case_*.json 2>/dev/null | wc -l | tr -d ' ' || true)"
case_count="$(find "${report_dir}" -name '*case_*.json' | wc -l | tr -d ' ')"
if [ "${case_count}" = "0" ]; then
    echo "一条用例都没跑完，看 wstest 的输出：" >&2
    tail -20 "${work_dir}/wstest.log" >&2
    exit 1
fi

echo "跑了 ${case_count} 条，FAILED ${failed_count} 条；报告在 ${report_dir}"
if [ "${failed_count}" != "0" ]; then
    for path in $(grep -l '"behavior": "FAILED"' "${report_dir}"/*case_*.json); do
        sed -n 's/^ *"id": "\(.*\)",/\1/p; s/^ *"result": "\(.*\)",/\1/p' "${path}" | paste -sd ' | ' -
    done
    exit 1
fi
echo "Autobahn 全部判据通过"
