#!/usr/bin/env bash
set -euo pipefail

# WebSocket 一致性验收：用 Autobahn|Testsuite（另一家独立实现，517 条用例）打一个正在跑的
# ReferenceServer 的 WebSocket 路由。
#
# 为什么要引外部裁判：自家探针与实现同源，查不出「两边都错在同一种理解上」。Autobahn 每条用例
# 给的是「期望的事件序列」，本机这一轮就是这样查出两件事的——
#   * 1.2.x / 9.2.x / 12.2.x 等三十六上：回显不分类型一律发文本帧，本端编码器随即按 1007 拒发
#     （文本负载必须是合法 UTF-8，RFC 6455 §5.6），连接断在对端等回显的时候；
#   * 9.1.4 / 9.3.9：单帧上限(1 MiB)比消息上限(8 MiB)还小，于是「拆片收得下、整片发来回 1009」。
#
# 用法：先把服务端起起来（回显要按原类型回帧），再跑本脚本
#   build/release/samples/ReferenceServer --port 18081 --threads 2 --metrics &
#   scripts/websocket_autobahn.sh 18081
#
# 镜像：默认用官方 crossbario/autobahn-testsuite。取不到 Docker Hub 的环境（本机过去就是）可以用
# 自备镜像：按官方 docker/Dockerfile 里的 py2.7 依赖装一份，再 AUTOBAHN_IMAGE=... 指过来。
# 两侧都按镜像自己的 PATH 解析 wstest（不在命令行里写死它的安装路径）：官方 latest 在 2025-10
# 那次重建里把 wstest 从 /usr/local/bin 挪到了 /opt/pypy/bin，写死路径的调用当场就起不来。
#
# 可选项：
#   AUTOBAHN_IMAGE      镜像名（默认 crossbario/autobahn-testsuite:latest）
#   AUTOBAHN_CASES      JSON 数组字面量，默认 '["*"]'；只跑几条快速回归时就填具体号
#   AUTOBAHN_EXCLUDE    逗号分隔的用例号，默认排掉超容量的那两条（见下）
#   AUTOBAHN_REPORT_DIR 报告落点，默认仓库外的临时目录
#   AUTOBAHN_MIN_CASES  用例面下限，默认 500（官方镜像 517 条减去默认排除的 2 条，实测裁判自己
#                       打「Ok, will run 515 test cases」）；落盘的报告数低于它就退 2——裁判提前
#                       收尾时交出的那几十份照样全绿，不数这一格就看不出覆盖面掉了
#   AUTOBAHN_ATTEMPTS   裁判最多跑几次，默认 2。这个数被时间预算钉住，不是随手取的：一次跑完实测
#                       21m38s（2026-10-10 那轮 develop，从镜像拉好到 515 份落盘），作业自己的
#                       timeout-minutes 是 60，两次加退避约 47 分钟还留余量，三次就要顶到 69 分钟
#                       以上——那样红的就不是这条判据而是「runner 取消作业」。要加次数就连着
#                       timeout-minutes 一起调
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
    # 这条探测在 MSYS 下有两种失败原因，把它们分开说：`-o /dev/null` 依赖 MSYS 的路径换算，
    # 而 MSYS_NO_PATHCONV=1 会把换算关掉——native curl.exe 于是把 /dev/null 当成一个真文件去建，
    # 建不出来就退 23，看起来却像「服务端没起」。本脚本的 docker 调用自己带了需要的开关，
    # 不要从外面设 MSYS_NO_PATHCONV 跑它。
    if [ -n "${MSYS_NO_PATHCONV:-}" ]; then
        echo "提示：别用 MSYS_NO_PATHCONV=1 跑本脚本——那样 native curl 写不了 /dev/null，这条探测必然假失败" >&2
    fi
    echo "没人在 ${port} 上应答：先把 ReferenceServer 起起来（--port ${port} --metrics），再来跑本脚本" >&2
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
      {"url": "ws://host.docker.internal:${port}${ws_path}", "agent": "AsynGyanis ReferenceServer"}
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

# 拉镜像排在跑裁判之前，并给单次尝试加时限：runner 到 Docker Hub 的 token 请求会整片超时
# （2026-10-09 那一轮 h2spec/Autobahn 与 Redis 两档就是这么红的），一次超时不等于「裁判不存在」。
# 三次都不成才退 2，明说「拿不到裁判」——退 1 会被读成服务端有问题，那条出口上面已经写过分档
pull_attempt=0
until timeout 240 docker pull "${image}"; do
    pull_attempt=$((pull_attempt + 1))
    if [ "${pull_attempt}" -ge 3 ]; then
        echo "三次拉取 ${image} 都没成功（Docker Hub 侧超时），本档按「裁判取不到」退出" >&2
        exit 2
    fi
    wait_seconds=$((pull_attempt * 10))
    echo "第 ${pull_attempt} 次拉取 ${image} 失败，${wait_seconds} 秒后重试" >&2
    sleep "${wait_seconds}"
done

# MSYS_NO_PATHCONV：Git Bash 会把以 / 开头的参数（这里是容器内路径）换算成 Windows 路径，
# 换算完容器里就没有这个文件了。挂载源已用 pwd -W 写成 Windows 形式，无需再换算。
# 不再写 --entrypoint /usr/local/bin/wstest：官方 latest 镜像把它从 /usr/local/bin 搬到了
# /opt/pypy/bin（镜像的 CMD 用的是裸名 wstest，说明它在 PATH 里），猜安装路径不如让镜像自己解析。
# --add-host 是给 Linux 侧（CI runner）用的：那里默认没有 host.docker.internal 这一条，
# 不加就是「裁判起来了但连不上服务端」，一条用例都跑不完
min_cases="${AUTOBAHN_MIN_CASES:-500}"
# 官方镜像的 WS 服务端用例是 517 条，扣掉本脚本默认排除的 2 条得 515（裁判自己打的「Ok, will run
# 515 test cases」就是这条算式），留 15 条余量给上游版本的小幅增减；要按自己的用例面收紧或放宽，
# 用 AUTOBAHN_MIN_CASES 覆盖。只判「有没有报告」不够：报告是收尾才整体落盘的，裁判被掐死时一份
# 都没有，而它中途只跑掉 52 条就跑完收尾时，那 52 份照样全绿——52 与 515 在旧判据里没有区别。
wstest_attempts="${AUTOBAHN_ATTEMPTS:-2}"
attempt=0
while :; do
    attempt=$((attempt + 1))
    rm -rf "${report_dir:?}"/*
    wstest_rc=0
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' docker run --rm -i \
        --add-host=host.docker.internal:host-gateway \
        -v "$(mount_source "${work_dir}")/fuzzingclient.json:/fuzzingclient.json:ro" \
        -v "$(mount_source "${report_dir}"):/reports" \
        "${image}" wstest -m fuzzingclient -s /fuzzingclient.json > "${work_dir}/wstest.log" 2>&1 || wstest_rc=$?

    failed_count="$(grep -l '"behavior": "FAILED"' "${report_dir}"/*case_*.json 2>/dev/null | wc -l | tr -d ' ' || true)"
    case_count="$(find "${report_dir}" -name '*case_*.json' | wc -l | tr -d ' ')"
    attempted="$(grep -ac 'Running test case ID' "${work_dir}/wstest.log" 2>/dev/null || true)"
    echo "第 ${attempt} 次跑裁判：落盘 ${case_count} 份报告、分派 ${attempted:-0} 条、wstest 退出码 ${wstest_rc}（用例面下限 ${min_cases}）"

    if [ "${case_count}" -ge "${min_cases}" ]; then
        break
    fi

    if [ "${attempt}" -ge "${wstest_attempts}" ]; then
        # 退 2 而不是退 1：这一档的红说的是「裁判没跑完」，不是「服务端有规范问题」。两种成因的
        # 下一步完全不同——前者重跑或换 runner，后者才去查 WebSocket 实现
        echo "裁判连跑 ${wstest_attempts} 次都没交够用例面（最后一轮落盘 ${case_count} 份、分派 ${attempted:-0} 条、退出码 ${wstest_rc}）" >&2
        if [ "${attempted:-0}" = "0" ]; then
            echo "一条都没分派：这才像裁判连不上服务端（先确认 ${port} 上真的在听、路径是 ${ws_path}）" >&2
            echo "---- wstest.log 开头 12 行（镜像与连接问题在这）----" >&2
            head -12 "${work_dir}/wstest.log" >&2
        else
            echo "分派过 ${attempted:-0} 条却没跑完：退出码 137/143 这类是 runner 把容器掐了（2026-10-10 实测同一份镜像摘要下 515→52→0），不是服务端的规范失败" >&2
        fi
        echo "---- wstest.log 结尾 20 行（断在哪儿在这）----" >&2
        tail -20 "${work_dir}/wstest.log" >&2
        exit 2
    fi

    wait_seconds=$((attempt * 20))
    echo "落盘 ${case_count} 份低于下限 ${min_cases}，${wait_seconds} 秒后重跑裁判" >&2
    sleep "${wait_seconds}"
done
echo "用例面核对通过：${case_count} 条 ≥ 下限 ${min_cases}（第 ${attempt} 次跑成，分派 ${attempted:-0} 条）"

if [ "${failed_count}" != "0" ]; then
    for path in $(grep -l '"behavior": "FAILED"' "${report_dir}"/*case_*.json); do
        sed -n 's/^ *"id": "\(.*\)",/\1/p; s/^ *"result": "\(.*\)",/\1/p' "${path}" | paste -sd ' | ' -
    done
    exit 1
fi
echo "Autobahn 全部判据通过"
