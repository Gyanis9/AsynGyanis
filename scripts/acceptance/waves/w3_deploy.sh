#!/usr/bin/env bash
# W3 部署形态：ReferenceServer 的 h1/h2/h3、worker 补位、SIGHUP 与 systemd 单元检查。
set -euo pipefail

build="${1:?用法: w3_deploy.sh <build-dir> <evidence-dir>}"
evidence="${2:?用法: w3_deploy.sh <build-dir> <evidence-dir>}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
mkdir -p "$evidence"

dry_run="${ASYN_ACCEPTANCE_DRY_RUN:-0}"
pids=()
cleanup() {
    for pid in "${pids[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT

if [[ "$dry_run" == "1" ]]; then
    echo "[dry-run] ReferenceServer h1/h2c/TLS/h3/workers 起停与端点检查"
    echo "[dry-run] CoreUpgrade"
    echo "[dry-run] systemd-analyze verify packaging/asyngyanis.service.example"
    exit 0
fi

reference="$build/samples/ReferenceServer"
certificate="$repo/tests/Core/fixtures/test_ip_cert.pem"
private_key="$repo/tests/Core/fixtures/test_ip_key.pem"
if [[ ! -x "$reference" ]]; then
    echo "找不到可执行体 $reference" >&2
    exit 2
fi
if ! command -v systemd-analyze >/dev/null 2>&1; then
    echo "systemd-analyze 不可用" >&2
    exit 2
fi

wait_http_h1() {
    local port="$1"
    local attempt
    for attempt in $(seq 1 30); do
        if curl -fsS --max-time 1 "http://127.0.0.1:${port}/" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    echo "端口 ${port} 在 30 秒内没有就绪" >&2
    return 1
}

wait_http_h2c() {
    local port="$1"
    local attempt
    for attempt in $(seq 1 30); do
        if curl -fsS --http2-prior-knowledge --max-time 1 "http://127.0.0.1:${port}/" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    echo "h2c 端口 ${port} 在 30 秒内没有就绪" >&2
    return 1
}

wait_https() {
    local port="$1"
    local attempt
    for attempt in $(seq 1 30); do
        if curl --cacert "$certificate" -fsS --max-time 1 "https://127.0.0.1:${port}/" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    echo "TLS 端口 ${port} 在 30 秒内没有就绪" >&2
    return 1
}

"$reference" --port 18090 --threads 1 --metrics >"$evidence/h1-server.log" 2>&1 &
pids+=("$!")
wait_http_h1 18090

for path in / /json /bench /big; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:18090${path}")
    test "$code" = "200" || { echo "${path} 返回 ${code}" >&2; exit 1; }
done
curl -sS "http://127.0.0.1:18090/sse" >"$evidence/sse.txt"
grep -q "data: one" "$evidence/sse.txt"
grep -q "data: two" "$evidence/sse.txt"

"$reference" --port 18091 --threads 1 --metrics --h2c >"$evidence/h2c-server.log" 2>&1 &
pids+=("$!")
wait_http_h2c 18091
curl -sS --http2-prior-knowledge -o /dev/null -w '%{http_version}\n' "http://127.0.0.1:18091/bench" >"$evidence/h2c-version.txt"
grep -q '^2$' "$evidence/h2c-version.txt"

"$reference" --port 18444 --threads 1 --https --cert "$certificate" --key "$private_key" >"$evidence/tls-server.log" 2>&1 &
pids+=("$!")
wait_https 18444
curl --cacert "$certificate" -sS --http2 -o /dev/null -w '%{http_version}\n' "https://127.0.0.1:18444/bench" >"$evidence/h2tls-version.txt"
grep -q '^2$' "$evidence/h2tls-version.txt"

"$reference" --port 18445 --threads 1 --https --h3 --cert "$certificate" --key "$private_key" >"$evidence/h3-server.log" 2>&1 &
pids+=("$!")
attempt=0
until python3 "$repo/scripts/h3_acceptance.py" 127.0.0.1 18445 /bench 200 >"$evidence/h3-bench.log" 2>&1; do
    attempt=$((attempt + 1))
    if [[ $attempt -ge 30 ]]; then
        echo "h3 在 30 秒内没有就绪" >&2
        exit 1
    fi
    sleep 1
done
if ! python3 "$repo/scripts/h3_acceptance.py" 127.0.0.1 18445 /ws 200 --websocket "hello" >"$evidence/h3-ws.log" 2>&1; then
    echo "h3 WebSocket 隧道验收失败，详见 $evidence/h3-ws.log" >&2
    exit 1
fi

"$reference" --port 18092 --threads 1 --workers 2 >"$evidence/workers.log" 2>&1 &
master_pid=$!
pids+=("$master_pid")
wait_http_h1 18092
worker_count=$(ps --ppid "$master_pid" -o pid= | wc -l | tr -d ' ')
test "$worker_count" -ge 2 || { echo "worker 数不足：${worker_count}" >&2; exit 1; }
first_worker=$(ps --ppid "$master_pid" -o pid= | head -n 1 | tr -d ' ')
kill -9 "$first_worker"
for attempt in $(seq 1 20); do
    code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:18092/" || true)
    if [[ "$code" = "200" ]]; then
        break
    fi
    sleep 1
done
test "$code" = "200" || { echo "杀掉 worker 后服务未恢复" >&2; exit 1; }

kill -HUP "$master_pid"
sleep 1
kill -0 "$master_pid"
code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:18092/")
test "$code" = "200"

"$build/samples/CoreUpgrade" >"$evidence/core-upgrade.log" 2>&1

unit_copy="$evidence/asyngyanis.service"
cp "$repo/packaging/asyngyanis.service.example" "$unit_copy"
systemd_report=$(systemd-analyze verify "$unit_copy" 2>&1 || true)
printf '%s\n' "$systemd_report" >"$evidence/systemd-verify.log"
unexpected=$(printf '%s\n' "$systemd_report" | grep -av "ReferenceServer is not executable" | sed '/^$/d' || true)
test -z "$unexpected" || { echo "systemd 单元报告非空：" >&2; printf '%s\n' "$unexpected" >&2; exit 1; }

echo "W3 全部检查通过"
