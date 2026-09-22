#!/usr/bin/env python3
"""HTTP/3 取消探针：用 aioquic 把一批请求流以 RESET_STREAM 与 STOP_SENDING 打断，核对连接活到最后。

服务端在一条对端流的两侧都收口之后会把记录摘掉。摘早了会把合法的重传判成越界、把整条连接杀掉；
摘漏了只是记账一直涨。两种错在自写的单元对端里都看不全，这里拿另一个实现当真对端：连发两轮
打断型请求，再补一条正常 GET——收尾那条拿到 200 且全程没被连接级错误打断，才算过。

用法：
    python3 h3_cancellation_probe.py <host> <port> <path> [每类打断的条数]

<path> 要写成 path-absolute（以 / 开头）：服务端按 RFC 9114 §4.3.1 判它，写成 "12" 这种
会被回 400，而探针把 400 报成「收尾请求没拿到 200」，看不出是参数给错了。

退出码 0 表示通过；非 0 打印原因。依赖：pip install aioquic
"""

import asyncio
import os
import sys

# 与 h3_acceptance.py 同目录：连接参数与等待口径复用一份，避免两个探针各写一遍再漂移
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from aioquic.asyncio.client import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived

from h3_acceptance import (
    HANDSHAKE_TIMEOUT_SECONDS,
    SETTINGS_SETTLE_SECONDS,
    make_quic_configuration,
)

# RFC 9114 §8.1 的 H3_REQUEST_CANCELLED
H3_REQUEST_CANCELLED = 0x0101


class CancellationProbe(QuicConnectionProtocol):
    """会主动打断自己请求流的 h3 客户端：只盯最后那条正常请求的响应状态。"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._http = H3Connection(self._quic)
        self.watch_stream_id = None
        self.watch_status = None
        self.answered = asyncio.Event()

    def request_headers(self, authority, path):
        return [
            (b":method", b"GET"),
            (b":scheme", b"https"),
            (b":authority", authority.encode()),
            (b":path", path.encode()),
        ]

    def send_reset_get(self, authority, path):
        """发一条不收尾的 GET，随即复位本端的发送侧：服务端看到的是一条入站流被打断。"""
        stream_id = self._quic.get_next_available_stream_id()
        self._http.send_headers(stream_id=stream_id, headers=self.request_headers(authority, path),
                                end_stream=False)
        # 复位要在头块之后、在同一拍里发出去：服务端得先见到请求头，再见到 RESET_STREAM
        self._quic.reset_stream(stream_id, H3_REQUEST_CANCELLED)
        self.transmit()
        return stream_id

    def send_stopped_get(self, authority, path):
        """发一条完整的 GET，随即请对端停发：服务端看到的是一次叫停，它要回 RESET_STREAM。"""
        stream_id = self._quic.get_next_available_stream_id()
        self._http.send_headers(stream_id=stream_id, headers=self.request_headers(authority, path),
                                end_stream=True)
        self.transmit()
        self._quic.stop_stream(stream_id, H3_REQUEST_CANCELLED)
        self.transmit()
        return stream_id

    def send_plain_get(self, authority, path):
        """发一条正常 GET 并盯住它的状态码：连接若被前面的打断弄坏，这条就拿不到响应。"""
        self.watch_stream_id = self._quic.get_next_available_stream_id()
        self._http.send_headers(stream_id=self.watch_stream_id, headers=self.request_headers(authority, path),
                                end_stream=True)
        self.transmit()

    def quic_event_received(self, event):
        for http_event in self._http.handle_event(event):
            if not isinstance(http_event, (HeadersReceived, DataReceived)):
                continue
            if http_event.stream_id != self.watch_stream_id:
                continue
            if isinstance(http_event, HeadersReceived):
                for name, value in http_event.headers:
                    if name == b":status":
                        self.watch_status = int(value)
            # 头块与正文事件都要认：收尾那条 GET 带正文，只盯头块的收尾标记会一直等不到
            if http_event.stream_ended:
                self.answered.set()


async def run_probe(host, port, path, cancellation_count):
    """连发两轮打断型请求，再补一条正常 GET，返回那条 GET 的状态码。"""
    configuration = make_quic_configuration()
    authority = f"{host}:{port}"
    async with connect(host, port, configuration=configuration, create_protocol=CancellationProbe) as client:
        await asyncio.sleep(SETTINGS_SETTLE_SECONDS)
        for _ in range(cancellation_count):
            client.send_reset_get(authority, path)
        for _ in range(cancellation_count):
            client.send_stopped_get(authority, path)
        # 打断与正常请求之间留一拍：让服务端有机会把已收口的记录摘掉，再在它之上开新流
        await asyncio.sleep(0.2)
        client.send_plain_get(authority, path)
        await asyncio.wait_for(client.answered.wait(), timeout=HANDSHAKE_TIMEOUT_SECONDS)
        return client.watch_status


def main():
    if len(sys.argv) < 4:
        print("用法：h3_cancellation_probe.py <host> <port> <path> [每类打断的条数]", file=sys.stderr)
        return 2

    host = sys.argv[1]
    port = int(sys.argv[2])
    path = sys.argv[3]
    # 参数写错时别把服务端的正确判定报成缺陷：服务端按 RFC 9114 §4.3.1 只收 path-absolute，
    # 把「每类打断的条数」误填成第三个实参会让收尾请求拿到 400，看起来像服务端出了问题
    if not path.startswith("/"):
        print(f"探针参数非法：<path> 要写成以 / 开头的 path-absolute，实得「{path}」", file=sys.stderr)
        return 2
    cancellation_count = int(sys.argv[4]) if len(sys.argv) > 4 else 20

    try:
        status = asyncio.run(run_probe(host, port, path, cancellation_count))
    except Exception as probe_error:  # noqa: BLE001 - 探针要把任何失败原因如实报出来
        print(f"{2 * cancellation_count} 条打断之后连接已不可用：{type(probe_error).__name__}: {probe_error}",
              file=sys.stderr)
        return 1

    print(f"打断 {2 * cancellation_count} 条之后收尾请求 status={status}")
    if status != 200:
        print(f"收尾请求没有拿到 200：实得 {status}", file=sys.stderr)
        return 1
    print("HTTP/3 取消探针通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
