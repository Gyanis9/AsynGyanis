#!/usr/bin/env bash
# W6 可观测性：起 OTLP 接收器与 ReferenceServer，抓取四个端点并核对三路 OTLP。
set -euo pipefail

build="${1:?用法: w6_observability.sh <build-dir> <evidence-dir>}"
evidence="${2:?用法: w6_observability.sh <build-dir> <evidence-dir>}"
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
    echo "[dry-run] python3 scripts/acceptance/otlp_collector.py --port 19090"
    echo "[dry-run] ReferenceServer --config w6.yaml --port 18095 --threads 1 --metrics --h2c"
    echo "[dry-run] curl /metrics /healthz /readyz /debug/loops"
    exit 0
fi

reference="$build/samples/ReferenceServer"
if [[ ! -x "$reference" ]]; then
    echo "找不到可执行体 $reference" >&2
    exit 2
fi

collector_log="$evidence/collector.log"
otlp_log="$evidence/otlp.jsonl"
config="$evidence/w6.yaml"
cat >"$config" <<'YAML'
metrics:
  enabled: true
  service_name: asyngyanis-acceptance
  export_interval_ms: 500
  otlp:
    endpoint: http://127.0.0.1:19090/v1/metrics
tracing:
  enabled: true
  service_name: asyngyanis-acceptance
  sample_ratio: 1.0
  export_interval_ms: 500
  otlp:
    endpoint: http://127.0.0.1:19090/v1/traces
log_export:
  enabled: true
  service_name: asyngyanis-acceptance
  export_interval_ms: 500
  otlp:
    endpoint: http://127.0.0.1:19090/v1/logs
YAML

python3 "$repo/scripts/acceptance/otlp_collector.py" --port 19090 --out "$otlp_log" >"$collector_log" 2>&1 &
pids+=("$!")
sleep 1

"$reference" --config "$config" --port 18095 --threads 1 --metrics >"$evidence/server.log" 2>&1 &
pids+=("$!")
ready=0
for attempt in $(seq 1 30); do
    if curl -fsS --max-time 1 "http://127.0.0.1:18095/" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
done
test "$ready" = "1" || { echo "W6 业务口在 30 秒内没有就绪" >&2; exit 1; }

for attempt in $(seq 1 10); do
    curl -fsS "http://127.0.0.1:18095/bench" >/dev/null
done
curl -fsS "http://127.0.0.1:18095/metrics" >"$evidence/metrics.txt"
curl -fsS "http://127.0.0.1:18095/healthz" >"$evidence/healthz.txt"
curl -fsS "http://127.0.0.1:18095/readyz" >"$evidence/readyz.txt"
curl -fsS "http://127.0.0.1:18095/debug/loops" >"$evidence/debug-loops.json"

grep -q "asyn_http" "$evidence/metrics.txt"
grep -q '"loops"' "$evidence/debug-loops.json"
sleep 3

for path in /v1/metrics /v1/logs /v1/traces; do
    grep -q "\"path\": \"${path}\"" "$otlp_log" || { echo "collector 没收到 ${path}" >&2; exit 1; }
done

echo "W6 可观测性检查通过"
