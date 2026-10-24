"""记录 OTLP/HTTP JSON 的 POST 路径与字节数，供可观测性验收对收。"""

from __future__ import annotations

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

OTLP_PATHS = {"/v1/metrics", "/v1/logs", "/v1/traces"}


def is_otlp_path(path: str) -> bool:
    return path in OTLP_PATHS


def make_record(path: str, body: bytes) -> dict:
    return {"path": path, "bytes": len(body)}


class Handler(BaseHTTPRequestHandler):
    server_version = "AsynAcceptanceCollector/1.0"

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        record = make_record(self.path, body)
        with self.server.output_lock:
            with self.server.output_path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(record, ensure_ascii=False) + "\n")
        status = 200 if is_otlp_path(self.path) else 404
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self) -> None:
        self.send_response(200 if self.path == "/healthz" else 404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, format_string: str, *args) -> None:
        del format_string, args


def serve(port: int, output_path: Path, ready_event: threading.Event) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.output_path = output_path
    server.output_lock = threading.Lock()
    ready_event.set()
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description="OTLP/HTTP 验收接收器")
    parser.add_argument("--port", type=int, default=19090)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    output_path = Path(args.out)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("", encoding="utf-8")
    ready = threading.Event()
    server = serve(args.port, output_path, ready)
    print(f"collector listening on 127.0.0.1:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
