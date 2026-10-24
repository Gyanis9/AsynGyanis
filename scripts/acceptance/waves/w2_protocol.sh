#!/usr/bin/env bash
# W2 协议面：起 ReferenceServer，跑 h2spec 两轮与 WebSocket 互操作探针。
set -euo pipefail

build="${1:?用法: w2_protocol.sh <build-dir> <evidence-dir>}"
evidence="${2:?用法: w2_protocol.sh <build-dir> <evidence-dir>}"
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

certificate="$repo/tests/Core/fixtures/test_ip_cert.pem"
private_key="$repo/tests/Core/fixtures/test_ip_key.pem"

if [[ "$dry_run" == "1" ]]; then
    h2spec_bin="${H2SPEC_BIN:-h2spec}"
    echo "[dry-run] $build/samples/ReferenceServer --port 18080 --threads 1 --metrics --h2c"
    echo "[dry-run] $build/samples/ReferenceServer --port 18081 --threads 1 --metrics"
    echo "[dry-run] $build/samples/ReferenceServer --port 18443 --threads 1 --https --cert $certificate --key $private_key"
    echo "[dry-run] $h2spec_bin -h 127.0.0.1 -p 18080 --strict"
    echo "[dry-run] $h2spec_bin -h 127.0.0.1 -p 18443 --tls --insecure"
    echo "[dry-run] python3 $repo/scripts/ws_interop_probe.py 127.0.0.1 18081"
    echo "[dry-run] python3 $repo/scripts/ws_interop_probe.py 127.0.0.1 18443 --wss"
    exit 0
fi

h2spec_bin="${H2SPEC_BIN:-$(command -v h2spec || true)}"
if [[ -z "$h2spec_bin" ]]; then
    echo "找不到 h2spec：设置 H2SPEC_BIN 或把它放进 PATH" >&2
    exit 2
fi

reference="$build/samples/ReferenceServer"
if [[ ! -x "$reference" ]]; then
    echo "找不到可执行体 $reference" >&2
    exit 2
fi

h2c_log="$evidence/h2c-server.log"
tls_log="$evidence/tls-server.log"
plain_log="$evidence/plain-server.log"

"$reference" --port 18080 --threads 1 --metrics --h2c >"$h2c_log" 2>&1 &
pids+=("$!")
"$reference" --port 18081 --threads 1 --metrics >"$plain_log" 2>&1 &
pids+=("$!")
"$reference" --port 18443 --threads 1 --https --cert "$certificate" --key "$private_key" >"$tls_log" 2>&1 &
pids+=("$!")

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

wait_http_h2c 18080
wait_http_h1 18081
wait_https 18443

status=0
"$h2spec_bin" -h 127.0.0.1 -p 18080 --strict >"$evidence/h2spec-h2c.log" 2>&1 || status=1
"$h2spec_bin" -h 127.0.0.1 -p 18443 --tls --insecure >"$evidence/h2spec-tls.log" 2>&1 || status=1
python3 "$repo/scripts/ws_interop_probe.py" 127.0.0.1 18081 >"$evidence/ws-plain.log" 2>&1 || status=1
python3 "$repo/scripts/ws_interop_probe.py" 127.0.0.1 18443 --wss >"$evidence/ws-wss.log" 2>&1 || status=1

tail -n 20 "$evidence/h2spec-h2c.log"
tail -n 20 "$evidence/h2spec-tls.log"
exit "$status"
