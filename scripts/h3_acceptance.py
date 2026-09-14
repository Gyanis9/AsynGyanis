#!/usr/bin/env python3
"""HTTP/3 验收探针：用 aioquic 这个独立实现向本框架的 h3 服务端发一条 GET，核对状态码与正文。

用独立实现当对端，比自写探针强：两边都按 RFC 9114/9000 办事，谁理解错了都会在这里露出来。

用法：
    python3 h3_acceptance.py <host> <port> <path> [期望状态码] [正文应包含的片段]
退出码 0 表示核对通过；非 0 打印原因后退出。

依赖：pip install aioquic
"""

import asyncio
import ssl
import sys

from aioquic.asyncio.client import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration

# 握手与响应的等待上限：本探针只用来验收，超时即判失败，不做重试
HANDSHAKE_TIMEOUT_SECONDS = 10


class Http3Probe(QuicConnectionProtocol):
    """一条 h3 客户端连接：发一条请求、把响应收全。"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._http = H3Connection(self._quic)
        self._stream_id = None
        self.status = None
        self.headers = {}
        self.body = bytearray()
        self.finished = asyncio.Event()

    def send_get(self, authority, path):
        """在一条新的双向流上发一条 GET（请求立即收尾，无正文）。"""
        self._stream_id = self._quic.get_next_available_stream_id()
        self._http.send_headers(
            stream_id=self._stream_id,
            headers=[
                (b":method", b"GET"),
                (b":scheme", b"https"),
                (b":authority", authority.encode()),
                (b":path", path.encode()),
            ],
            end_stream=True,
        )
        self.transmit()

    def quic_event_received(self, event):
        for http_event in self._http.handle_event(event):
            if not isinstance(http_event, (HeadersReceived, DataReceived)):
                continue
            if http_event.stream_id != self._stream_id:
                continue
            if isinstance(http_event, HeadersReceived):
                for name, value in http_event.headers:
                    if name == b":status":
                        self.status = int(value)
                    else:
                        self.headers[name.decode()] = value.decode()
                # 无正文的响应在这里就结束了
                if getattr(http_event, "stream_ended", False):
                    self.finished.set()
            else:
                self.body += http_event.data
                if getattr(http_event, "stream_ended", False):
                    self.finished.set()


async def run_probe(host, port, path):
    """跑完一条请求并返回探针（含状态码与正文）。"""
    configuration = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN)
    # 仓库内的自签证书：验收只关心协议互通，不校验链
    configuration.verify_mode = ssl.CERT_NONE

    async with connect(host, port, configuration=configuration, create_protocol=Http3Probe) as client:
        client.send_get(f"{host}:{port}", path)
        await asyncio.wait_for(client.finished.wait(), timeout=HANDSHAKE_TIMEOUT_SECONDS)
        return client


def main():
    if len(sys.argv) < 4:
        print("用法：h3_acceptance.py <host> <port> <path> [期望状态码] [正文片段]", file=sys.stderr)
        return 2

    host = sys.argv[1]
    port = int(sys.argv[2])
    path = sys.argv[3]
    expected_status = int(sys.argv[4]) if len(sys.argv) > 4 else 200
    expected_body = sys.argv[5] if len(sys.argv) > 5 else None

    try:
        client = asyncio.run(run_probe(host, port, path))
    except Exception as probe_error:  # noqa: BLE001 - 验收探针要把任何失败原因如实报出来
        print(f"HTTP/3 请求失败：{type(probe_error).__name__}: {probe_error}", file=sys.stderr)
        return 1

    body_text = client.body.decode(errors="replace")
    print(f"status={client.status} headers={client.headers} body={body_text!r}")

    if client.status != expected_status:
        print(f"状态码不符：期望 {expected_status}，实得 {client.status}", file=sys.stderr)
        return 1
    if expected_body is not None and expected_body not in body_text:
        print(f"正文里没有 {expected_body!r}", file=sys.stderr)
        return 1
    print("HTTP/3 验收通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
