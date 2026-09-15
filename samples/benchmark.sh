#!/bin/bash
set -e

BINARY="$(dirname "$0")/../build/debug/samples/echo_server"
PORT="${PORT:-8080}"

if [ ! -x "$BINARY" ]; then
    echo "找不到可执行文件 $BINARY；先构建 debug 目标再跑本脚本" >&2
    exit 1
fi

echo "=== Starting server ==="
"$BINARY" --port "$PORT" &
SERVER_PID=$!

# 脚本无论怎么退出（set -e 提前结束、Ctrl-C、正常收尾）都要把服务进程收走，
# 否则残留在后台占着端口，下一次运行会撞端口
cleanup() {
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

sleep 2

# Verify server is up
echo "=== Smoke test ==="
curl -s "http://localhost:$PORT/bench"
echo ""

echo ""
echo "=== ab: 10,000 requests, 100 concurrency, /bench ==="
ab -n 10000 -c 100 "http://localhost:$PORT/bench" 2>&1

echo ""
echo "=== ab: 5,000 requests, 50 concurrency, /json ==="
ab -n 5000 -c 50 "http://localhost:$PORT/json" 2>&1

echo ""
echo "=== Done ==="
