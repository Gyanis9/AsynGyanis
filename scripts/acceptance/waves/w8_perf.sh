#!/usr/bin/env bash
# W8 性能驱动：在 ECS 本机起各协议/开销对服务器，按阶段跑 perf_matrix。
# 用法：w8_perf.sh <build-dir> <evidence-dir> <python> [capacity|soak|all]
set -euo pipefail

build="${1:?用法: w8_perf.sh <build-dir> <evidence-dir> <python> [capacity|soak|all]}"
evidence="${2:?用法: w8_perf.sh <build-dir> <evidence-dir> <python> [capacity|soak|all]}"
python="${3:?用法: w8_perf.sh <build-dir> <evidence-dir> <python> [capacity|soak|all]}"
mode="${4:-all}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
reference="$build/samples/ReferenceServer"
certificate="$repo/tests/Core/fixtures/test_ip_cert.pem"
private_key="$repo/tests/Core/fixtures/test_ip_key.pem"
mkdir -p "$evidence/servers" "$evidence/www"
printf 'hello-performance\n' >"$evidence/www/greeting.txt"
cat >"$evidence/server.yaml" <<'YAML'
server:
  access_log: false
  limits:
    maximum_requests_per_connection: 0
YAML
cp "$evidence/server.yaml" "$evidence/tracing.yaml"
cat >>"$evidence/tracing.yaml" <<YAML
tracing:
  enabled: true
  service_name: asyng-performance
  sample_ratio: 1.0
  export_interval_ms: 500
  file:
    path: $evidence/traces.jsonl
YAML

pids=()
cleanup() {
    for pid in "${pids[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT

if [[ ! -x "$reference" ]]; then
    echo "找不到可执行体 $reference" >&2
    exit 2
fi

start_server() {
    local name="$1"
    shift
    local configuration="$evidence/server.yaml"
    if [[ "$name" == "tracing-on" ]]; then
        configuration="$evidence/tracing.yaml"
    fi
    "$reference" --config "$configuration" "$@" >"$evidence/servers/$name.log" 2>&1 &
    echo $!
}

wait_h1() {
    local port="$1"
    local attempt
    for attempt in $(seq 1 30); do
        if curl -fsS --max-time 1 "http://127.0.0.1:$port/" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    echo "端口 $port 在 30 秒内没有就绪" >&2
    return 1
}

wait_h2c() {
    local port="$1"
    local attempt
    for attempt in $(seq 1 30); do
        if curl -fsS --http2-prior-knowledge --max-time 1 "http://127.0.0.1:$port/" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    echo "h2c 端口 $port 在 30 秒内没有就绪" >&2
    return 1
}

wait_https() {
    local port="$1"
    local attempt
    for attempt in $(seq 1 30); do
        if curl --cacert "$certificate" -fsS --max-time 1 "https://127.0.0.1:$port/" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    echo "TLS 端口 $port 在 30 秒内没有就绪" >&2
    return 1
}

h1_pid=$(start_server h1 --port 18080 --threads 1 --metrics)
pids+=("$h1_pid")
h2c_pid=$(start_server h2c --port 18081 --threads 1 --metrics --h2c)
pids+=("$h2c_pid")
ws_pid=$(start_server ws --port 18082 --threads 1 --metrics)
pids+=("$ws_pid")
h3_pid=$(start_server h3 --port 18443 --threads 1 --https --h3 --metrics --cert "$certificate" --key "$private_key")
pids+=("$h3_pid")
metrics_off_pid=$(start_server metrics-off --port 18085 --threads 1)
pids+=("$metrics_off_pid")
metrics_on_pid=$(start_server metrics-on --port 18086 --threads 1 --metrics)
pids+=("$metrics_on_pid")
tracing_off_pid=$(start_server tracing-off --port 18087 --threads 1)
pids+=("$tracing_off_pid")
tracing_on_pid=$(start_server tracing-on --port 18088 --threads 1)
pids+=("$tracing_on_pid")
overhead_pid=$(start_server overhead-h1 --port 18090 --threads 1 --metrics --compress --static "$evidence/www")
pids+=("$overhead_pid")
overhead_tls_pid=$(start_server overhead-tls --port 18444 --threads 1 --https --metrics --compress --cert "$certificate" --key "$private_key")
pids+=("$overhead_tls_pid")

wait_h1 18080
wait_h2c 18081
wait_h1 18082
wait_https 18443
wait_h1 18085
wait_h1 18086
wait_h1 18087
wait_h1 18088
wait_h1 18090
wait_https 18444

overall_status=0
run_rows() {
    local rows="$1"
    shift
    local log_name status=0
    log_name="${rows//[^A-Za-z0-9]/_}"
    echo "[w8] 开始 $rows $(date -Is)"
    "$python" "$repo/scripts/acceptance/run_acceptance.py" \
        --runner ecs --build "$build" --evidence-root "$evidence" \
        --write-status --timeout 7200 --only "$rows" "$@" \
        >"$evidence/run-$log_name.log" 2>&1 || status=$?
    if [[ "$status" -ne 0 ]]; then
        overall_status="$status"
    fi
    if [[ -f "$evidence/results-all.json" ]]; then
        cp "$evidence/results-all.json" "$evidence/results-$log_name.json"
    fi
    echo "[w8] 结束 $rows exit=$status $(date -Is)"
}

if [[ "$mode" == "capacity" || "$mode" == "all" ]]; then
    run_rows "PERF.H1.CAPACITY,PERF.H2.CAPACITY,PERF.H2TLS.CAPACITY,PERF.H3.CAPACITY,PERF.WS.CAPACITY,PERF.BASELINE,PERF.OVERHEAD.TLS,PERF.OVERHEAD.COMPRESSION,PERF.OVERHEAD.METRICS,PERF.OVERHEAD.TRACING,PERF.OVERHEAD.STATIC"
fi

if [[ "$mode" == "recheck" ]]; then
    run_rows "PERF.H1.CAPACITY,PERF.H2.CAPACITY"
fi

if [[ "$mode" == "soak" || "$mode" == "all" ]]; then
    run_rows "PERF.SOAK.H1" --server-pid "$h1_pid"
    run_rows "PERF.SOAK.CHURN" --server-pid "$h1_pid"
    run_rows "PERF.SOAK.H2" --server-pid "$h2c_pid"
    run_rows "PERF.SOAK.H3" --server-pid "$h3_pid"
    run_rows "PERF.SOAK.WS" --server-pid "$ws_pid"
fi

echo "W8 完成：$mode exit=$overall_status"
echo "$mode done exit=$overall_status $(date -Is)" >"$evidence/w8-$mode.done"
exit "$overall_status"
