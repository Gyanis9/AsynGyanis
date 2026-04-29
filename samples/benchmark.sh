#!/bin/bash
set -e

BINARY="$(dirname "$0")/../build/debug/samples/echo_server"
PORT=8080

echo "=== Starting server ==="
"$BINARY" --port $PORT &
SERVER_PID=$!
sleep 2

# Verify server is up
echo "=== Smoke test ==="
curl -s http://localhost:$PORT/bench
echo ""

echo ""
echo "=== ab: 10,000 requests, 100 concurrency, /bench ==="
ab -n 10000 -c 100 http://localhost:$PORT/bench 2>&1

echo ""
echo "=== ab: 5,000 requests, 50 concurrency, /json ==="
ab -n 5000 -c 50 http://localhost:$PORT/json 2>&1

echo ""
echo "=== Done ==="
