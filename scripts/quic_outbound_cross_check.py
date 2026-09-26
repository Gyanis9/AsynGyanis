#!/usr/bin/env python3
"""QUIC 出站验收的对端：用 aioquic（独立实现）起一个回显服务端。

被测端是本框架的探针（tests/Tools/QuicProbeClient.cpp）。判据由 aioquic 的解析器给出：
它能把我们的 Initial 解成一条握手完成的连接、协商到 h3、并原样回显我们送出去的流数据，
才谈得上「两型实现一致」。本脚本只打印事实行，不做成功/失败断言——断言在
scripts/quic_outbound_cross_check.sh 里对两边的说法各核一遍。

输出协议（每行一条，立即 flush）：
  LISTENING <port>        已绑定的端口
  HANDSHAKE alpn=<名字>   对端握手完成与协商到的 ALPN（'-' 表示没谈成）
  RECEIVED <stream> <n>   从这条流收满 n 字节并已回显
  CLOSED <reason>         连接收口
  SERVER-EXIT             服务端退出
"""

import asyncio
import sys

from aioquic.asyncio import serve
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ConnectionTerminated, HandshakeCompleted, StreamDataReceived


class EchoProtocol(QuicConnectionProtocol):
    """把每条流上收到的字节原样送回，并打印事实行。"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._received_byte_counts = {}
        self._first_datagram_logged = False

    def datagram_received(self, data, addr):
        # 第一条报文单独打一行：ALPN 谈不拢的场景里不会有 HandshakeCompleted，也不保证有
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
        elif isinstance(event, StreamDataReceived):
            total = self._received_byte_counts.get(event.stream_id, 0) + len(event.data)
            self._received_byte_counts[event.stream_id] = total
            # 原样回显，并按对端的收尾状态一起收尾
            self._quic.send_stream_data(event.stream_id, event.data, end_stream=event.end_stream)
            self.transmit()
            if event.end_stream:
                print("RECEIVED %d %d" % (event.stream_id, total), flush=True)
        elif isinstance(event, ConnectionTerminated):
            print("CLOSED error_code=%s reason=%s"
                  % (getattr(event, "error_code", "?"), getattr(event, "reason_phrase", b"")), flush=True)


async def run(host, port, certificate, private_key, alpn, idle_seconds):
    configuration = QuicConfiguration(is_client=False)
    # 交路径而不是交 PEM 文本：load_cert_chain 自己读文件，证书与私钥成对校验
    configuration.load_cert_chain(certificate, private_key)
    configuration.alpn_protocols = [alpn]
    configuration.idle_timeout = idle_seconds

    server = await serve(host, port, configuration=configuration, create_protocol=EchoProtocol)
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
