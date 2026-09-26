#!/usr/bin/env python3
"""QUIC/HTTP/3 出站验收的对端：用 aioquic（独立实现）起一个真的 HTTP/3 服务端。

被测端是本框架的探针（tests/Tools/QuicProbeClient.cpp）。判据由 aioquic 的解析器给出：
它得能把我们的 Initial 解成一条握手完成的连接、协商到 h3、认出我们开的控制流与 QPACK 流、
把头段解成一条合法请求，才谈得上「两型实现一致」。本脚本只打印事实行，不做成功/失败断言——
断言在 scripts/quic_outbound_cross_check.sh 里对两边的说法各核一遍。

输出协议（每行一条，立即 flush）：
  LISTENING <port>        已绑定的端口
  INBOUND <n>             收到第一条报文（字节数）；ALPN 谈不拢时只有这一行
  HANDSHAKE alpn=<名字>   握手完成与协商到的 ALPN（'-' 表示没谈成）
  REQUEST <stream> <method> <path>   解出来的一条请求
  ANSWERED <stream> <n>   已把 n 字节的响应正文交给 aioquic 的编码器
  CLOSED ...              连接收口
  SERVER-EXIT             服务端退出
"""

import asyncio
import sys

from aioquic.asyncio import serve
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ConnectionTerminated, HandshakeCompleted

# 响应正文：探针按同样的字面串核对，改这里要一起改脚本里的期望长度
SERVED_BODY = b"aioquic-h3-served"


class H3ServerProtocol(QuicConnectionProtocol):
    """把一条请求答成 200 + 固定正文，并打印事实行。"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._http = H3Connection(self._quic)
        self._first_datagram_logged = False
        self._request_lines = {}

    def datagram_received(self, data, addr):
        # 第一条报文单独打一行：ALPN 谈不拢时不会有 HandshakeCompleted，也不保证有
        # ConnectionTerminated，「对面确实收到了我们的 Initial」只能靠这一步证明
        if not self._first_datagram_logged:
            self._first_datagram_logged = True
            print("INBOUND %d" % len(data), flush=True)
        super().datagram_received(data, addr)

    def quic_event_received(self, event):
        if isinstance(event, HandshakeCompleted):
            # alpn_protocol 的类型随 aioquic 版本而变（1.3 给 str，更早给 bytes），两种都接下来：
            # 判据是「对端协商到了什么」，不该因为一份版本差异而让整行丢掉
            negotiated = event.alpn_protocol
            if isinstance(negotiated, bytes):
                negotiated = negotiated.decode()
            print("HANDSHAKE alpn=" + (negotiated or "-"), flush=True)
        elif isinstance(event, ConnectionTerminated):
            print("CLOSED error_code=%s" % getattr(event, "error_code", "?"), flush=True)

        for http_event in self._http.handle_event(event):
            self.http_event_received(http_event)

    def http_event_received(self, event):
        if not isinstance(event, HeadersReceived) or event.stream_id in self._request_lines:
            return
        fields = {}
        for name, value in event.headers:
            key = name.decode() if isinstance(name, bytes) else name
            decoded = value.decode() if isinstance(value, bytes) else value
            fields.setdefault(key, decoded)
        method = fields.get(":method", "-")
        path = fields.get(":path", "-")
        self._request_lines[event.stream_id] = True
        print("REQUEST %d %s %s" % (event.stream_id, method, path), flush=True)

        headers = [
            (b":status", b"200"),
            (b"content-type", b"text/plain; charset=utf-8"),
            (b"x-served-by", b"aioquic"),
        ]
        # 头段先不带 END_STREAM：正文还在后面，收尾跟着最后一个 DATA。
        # （aioquic 1.3 的 HeadersReceived 上没有 end_stream 属性，请求是否已收尾这里用不上）
        self._http.send_headers(event.stream_id, headers, end_stream=False)
        self._http.send_data(event.stream_id, SERVED_BODY, end_stream=True)
        print("ANSWERED %d %d" % (event.stream_id, len(SERVED_BODY)), flush=True)
        self.transmit()


async def run(host, port, certificate, private_key, alpn, idle_seconds):
    configuration = QuicConfiguration(is_client=False)
    # 交路径而不是交 PEM 文本：load_cert_chain 自己读文件，证书与私钥成对校验
    configuration.load_cert_chain(certificate, private_key)
    configuration.alpn_protocols = [alpn]
    configuration.idle_timeout = idle_seconds

    server = await serve(host, port, configuration=configuration, create_protocol=H3ServerProtocol)
    print("LISTENING %d" % port, flush=True)
    # 没有事件可等，只能按时限睡：到点由外层 SIGTERM/本协程退出收场
    await asyncio.sleep(idle_seconds)
    server.close()


def main(argv):
    if len(argv) < 6:
        print("用法：quic_outbound_cross_check.py <地址> <端口> <证书> <私钥> <alpn> [时限秒]", file=sys.stderr)
        return 2
    host, port, certificate, private_key, alpn = argv[1], int(argv[2]), argv[3], argv[4], argv[5]
    idle_seconds = float(argv[6]) if len(argv) > 6 else 15.0
    try:
        asyncio.run(run(host, port, certificate, private_key, alpn, idle_seconds))
    except KeyboardInterrupt:
        pass
    print("SERVER-EXIT", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
