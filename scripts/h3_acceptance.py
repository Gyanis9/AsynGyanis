#!/usr/bin/env python3
"""HTTP/3 验收探针：用 aioquic 这个独立实现向本框架的 h3 服务端发一条 GET，核对状态码与正文。

用独立实现当对端，比自写探针强：两边都按 RFC 9114/9000 办事，谁理解错了都会在这里露出来。

用法：
    python3 h3_acceptance.py <host> <port> <path> [期望状态码] [正文应包含的片段]
    python3 h3_acceptance.py <host> <port> <path> --head
    python3 h3_acceptance.py <host> <port> <path> --stream [期望状态码] [正文片段]
    python3 h3_acceptance.py <host> <port> <path> --expect-header content-length=2 \
        --expect-header date
    python3 h3_acceptance.py <host> <port> <path> --websocket <帧负载文本>
    python3 h3_acceptance.py <host> <port> <path> --accept-encoding gzip [期望状态码] [正文片段]
第一条发普通 GET；--head 换成 HEAD（响应不许有正文）；--stream 在此之上再要求正文分趟到达且不带
content-length（流式响应的形状）；--expect-header 可重复，按名字数响应头（带 =值 时比取值）；
带 --websocket 时改成扩展 CONNECT（RFC 9220）隧道，在同一流上发两条 WebSocket 帧并核对回显。
带 --accept-encoding 时在请求里声明该编码，并按响应里的 content-encoding 把正文解回来再核对片段
（探针只带 gzip 解码器）。退出码 0 表示核对通过；非 0 打印原因后退出。

依赖：pip install aioquic
"""

import asyncio
import gzip
import os
import ssl
import sys

from aioquic.asyncio.client import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.logger import QuicFileLogger

# 握手与响应的等待上限：本探针只用来验收，超时即判失败，不做重试
HANDSHAKE_TIMEOUT_SECONDS = 10

# 发请求前先让服务端的 SETTINGS 到达：客户端本就该等对端 SETTINGS（RFC 9114 §6.2.1），
# 而 aioquic 1.3.0 没有公开的事件可等，探针里用一小段确定性的等待代替
SETTINGS_SETTLE_SECONDS = 0.5

# 置上非空值就把 QUIC 与 HTTP/3 事件全打出来：失败时能看清服务端发到哪一步
_trace_enabled = bool(os.environ.get("ASYN_H3_TRACE"))


class ProbeH3Connection(H3Connection):
    """知道「本端这条流发的是 HEAD」的 H3Connection。

    aioquic 的编解码器不记请求方法：它把响应里的 content-length 当成「后面该来这么多正文字节」，
    HEAD 的响应因此被它判成 `content-length does not match data size` 并作废整条连接。RFC 9110
    §9.3.2 明确允许 HEAD 给出 GET 会发出的那份头部而不带正文，因此探针在自己标记过的流上跳过这一项
    长度核对。跳过的只有这一处：帧的布局、QPACK 的解析、伪头齐备性与控制流规则仍由独立实现判定。
    """

    def __init__(self, quic, server=False):
        super().__init__(quic, server)
        # 本端在这几条流上发的是 HEAD：响应不许有正文
        self.head_stream_ids = set()

    def _check_content_length(self, stream):
        # self._stream 是 aioquic 内部的「流号 → H3Stream」表（1.3.0 的字段名）：换了版本它会直接
        # 抛 AttributeError，探针随之报错而不是静默放过，所以照着用即可
        if any(stream is self._stream.get(stream_id) for stream_id in self.head_stream_ids):
            return
        super()._check_content_length(stream)


class Http3Probe(QuicConnectionProtocol):
    """一条 h3 客户端连接：发一条请求、把响应收全。"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._http = ProbeH3Connection(self._quic)
        self._stream_id = None
        self.status = None
        self.headers = {}
        # 可重复头（Set-Cookie 一类）只有原始列表数得清条数：dict 会把同名的折叠成最后一条
        self.raw_headers = []
        self.body = bytearray()
        self.finished = asyncio.Event()
        # 收到过几个 DATA 事件：流式响应「分趟到达」的直接证据（拼起来的总字节数看不出分趟）
        self.data_event_count = 0
        # 隧道用：隧道不会收尾（没有 END_STREAM），因此按「收够这么多字节」判完成
        self.expected_byte_count = None
        self.enough_bytes = asyncio.Event()

    def send_get(self, authority, path, accept_encoding=None, method="GET"):
        """在一条新的双向流上发一条请求（请求立即收尾，无正文）。

        :param method: 方法原文；HEAD 走同一条路径，只是期望响应没有正文
        """
        self._stream_id = self._quic.get_next_available_stream_id()
        headers = [
            (b":method", method.encode()),
            (b":scheme", b"https"),
            (b":authority", authority.encode()),
            (b":path", path.encode()),
        ]
        # 带上 accept-encoding 才能验到压缩那一条链路：不声明的客户端本就不该收到编码正文
        if accept_encoding is not None:
            headers.append((b"accept-encoding", accept_encoding.encode()))
        if method == "HEAD":
            self._http.head_stream_ids.add(self._stream_id)
        self._http.send_headers(stream_id=self._stream_id, headers=headers, end_stream=True)
        self.transmit()

    def send_websocket_tunnel(self, authority, path, frames):
        """在一条新的双向流上发扩展 CONNECT（RFC 9220），随后在同一流上发 WebSocket 帧。

        :param frames: 逐条发出去的 WebSocket 帧字节（各发一个 DATA 帧）
        """
        self._stream_id = self._quic.get_next_available_stream_id()
        # 隧道请求不能收尾（end_stream=False）：收到 2xx 之后这条流还要跑 WebSocket 帧
        self._http.send_headers(
            stream_id=self._stream_id,
            headers=[
                (b":method", b"CONNECT"),
                (b":scheme", b"https"),
                (b":authority", authority.encode()),
                (b":path", path.encode()),
                (b":protocol", b"websocket"),
            ],
            end_stream=False,
        )
        for frame in frames:
            self._http.send_data(stream_id=self._stream_id, data=frame, end_stream=False)
        self.transmit()

    def quic_event_received(self, event):
        # ASYN_H3_TRACE 置上时把事件全打出来：失败时能看清服务端发到哪一步（比如 SETTINGS 有没有来）
        if _trace_enabled:
            print(f"[quic] {type(event).__name__} {event}", flush=True)
        for http_event in self._http.handle_event(event):
            if _trace_enabled:
                print(f"[http] {type(http_event).__name__} {http_event}", flush=True)
            if not isinstance(http_event, (HeadersReceived, DataReceived)):
                continue
            if http_event.stream_id != self._stream_id:
                continue
            if isinstance(http_event, HeadersReceived):
                self.raw_headers.extend(http_event.headers)
                for name, value in http_event.headers:
                    if name == b":status":
                        self.status = int(value)
                    else:
                        self.headers[name.decode()] = value.decode()
                # 无正文的响应在这里就结束了
                if getattr(http_event, "stream_ended", False):
                    self.finished.set()
            else:
                self.data_event_count += 1
                self.body += http_event.data
                if self.expected_byte_count is not None and len(self.body) >= self.expected_byte_count:
                    self.enough_bytes.set()
                if getattr(http_event, "stream_ended", False):
                    self.finished.set()


def make_masked_text_frame(text):
    """造一条带掩码的 WebSocket 文本帧（RFC 6455 §5.3：客户端帧必须带掩码）。"""
    payload = text.encode()
    mask = b"\x11\x22\x33\x44"
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    # 长度按 RFC 6455 §5.2 分三档编码：126 与 127 是「后面还跟着扩展长度」的哨兵，
    # 不能当成实际长度直接写进 7 位字段（那样超过 125 字节的负载会发出畸形帧）
    length = len(payload)
    if length <= 125:
        header = bytes([0x81, 0x80 | length])
    elif length <= 0xFFFF:
        header = bytes([0x81, 0x80 | 126]) + length.to_bytes(2, "big")
    else:
        header = bytes([0x81, 0x80 | 127]) + length.to_bytes(8, "big")
    return header + mask + masked


def make_server_text_frame(payload):
    """造一条不带掩码的服务端文本帧（RFC 6455 §5.2 的三档长度编码）。

    期望回显若直接手写帧头，超过 125 字节的负载会把 126/127 哨兵当成实际长度写进 7 位字段，
    解析出的长度与真实负载不符——探针会把「服务端其实答对了」误判成失败。
    """
    length = len(payload)
    if length <= 125:
        header = bytes([0x81, length])
    elif length <= 0xFFFF:
        header = bytes([0x81, 126]) + length.to_bytes(2, "big")
    else:
        header = bytes([0x81, 127]) + length.to_bytes(8, "big")
    return header + payload


def make_quic_configuration(idle_timeout_seconds=None):
    """客户端 QUIC 配置：ALPN 与证书校验口径全在这一处。

    :param idle_timeout_seconds: 客户端侧的空闲上限；不给就用 aioquic 的默认值。
        压测要「晾着连接让服务端自己收口」时会把它调小，让两端都在几秒内收掉。
    """
    configuration = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN)
    if idle_timeout_seconds is not None:
        configuration.idle_timeout = idle_timeout_seconds
    # 仓库内的自签证书：验收只关心协议互通，不校验链
    configuration.verify_mode = ssl.CERT_NONE
    # ASYN_H3_QLOG 给一个目录就把每一帧的收发都落盘（aioquic 自带 qlog）：验收失败时用它看清楚
    # 客户端到底发了什么、收到了什么
    if qlog_directory := os.environ.get("ASYN_H3_QLOG"):
        configuration.quic_logger = QuicFileLogger(qlog_directory)
    return configuration


async def run_probe(host, port, path, accept_encoding=None, method="GET"):
    """跑完一条请求并返回探针（含状态码与正文）。"""
    async with connect(host, port, configuration=make_quic_configuration(), create_protocol=Http3Probe) as client:
        # 先让服务端的 SETTINGS 到达再发请求（RFC 9114 §6.2.1 的口径）：aioquic 1.3.0 没有公开
        # 的 SETTINGS 事件可等，这里用一小段确定性的等待代替
        await asyncio.sleep(SETTINGS_SETTLE_SECONDS)
        client.send_get(f"{host}:{port}", path, accept_encoding, method)
        await asyncio.wait_for(client.finished.wait(), timeout=HANDSHAKE_TIMEOUT_SECONDS)
        return client


async def run_websocket_probe(host, port, path, text):
    """在一条扩展 CONNECT 隧道上发两条 WebSocket 帧，等回显回来。

    :param text: 两条（内容不同的）文本帧的负载来源：第一条用 text，第二条用 text + "!"
    :return: (探针, 期望的回显字节)
    """
    frames = [make_masked_text_frame(text), make_masked_text_frame(text + "!")]
    # 服务端回显的帧不带掩码（RFC 6455 §5.1）
    expected_echo = make_server_text_frame(text.encode()) + make_server_text_frame((text + "!").encode())

    async with connect(host, port, configuration=make_quic_configuration(), create_protocol=Http3Probe) as client:
        await asyncio.sleep(SETTINGS_SETTLE_SECONDS)
        client.expected_byte_count = len(expected_echo)
        client.send_websocket_tunnel(f"{host}:{port}", path, frames)
        await asyncio.wait_for(client.enough_bytes.wait(), timeout=HANDSHAKE_TIMEOUT_SECONDS)
        return client, expected_echo


def run_websocket_mode(host, port, path, text):
    """WebSocket 隧道验收：两条帧的回显要逐字节对上，并且隧道以 2xx 建立。"""
    try:
        client, expected_echo = asyncio.run(run_websocket_probe(host, port, path, text))
    except Exception as probe_error:  # noqa: BLE001 - 验收探针要把任何失败原因如实报出来
        print(f"HTTP/3 隧道请求失败：{type(probe_error).__name__}: {probe_error}", file=sys.stderr)
        return 1

    print(f"status={client.status} headers={client.headers} 回显 {len(client.body)} 字节")
    if not 200 <= client.status < 300:
        print(f"隧道没有以 2xx 建立：实得 {client.status}", file=sys.stderr)
        return 1
    if bytes(client.body) != expected_echo:
        print(f"回显与发出去的对不上：期望 {expected_echo!r}，实得 {bytes(client.body)!r}", file=sys.stderr)
        return 1
    print("HTTP/3 WebSocket 隧道验收通过")
    return 0


def check_expected_headers(client, expected_headers):
    """按名字核对响应头：同名多条时数条数，给了期望值就逐条比。

    :param client: 跑完的探针
    :param expected_headers: 形如 ``name`` 或 ``name=value`` 的期望串（value 允许是 *，只要求存在）
    :return: 不合期望的说明列表，空列表表示全部对上
    """
    problems = []
    for expectation in expected_headers:
        name, separator, value = expectation.partition("=")
        actual_values = [raw_value.decode() for raw_name, raw_value in client.raw_headers if raw_name.decode() == name]
        if not actual_values:
            problems.append(f"响应里没有头 {name!r}")
            continue
        if separator and value != "*":
            counts = {}
            for actual_value in actual_values:
                counts[actual_value] = counts.get(actual_value, 0) + 1
            if value not in counts:
                problems.append(f"头 {name!r} 的取值不符：期望 {value!r}，实得 {sorted(counts)}")
    return problems


def run_head_mode(host, port, path, expected_headers=None):
    """HEAD 验收：状态码与 content-length 都要在，正文一个字节都不许出现在线上。

    RFC 9110 §9.3.2 要 HEAD 的响应给出「GET 会给出的那份头部」而不带正文。严格的对端会把多出来的
    正文当成畸形响应，所以这条放在进程外判；本端自解自只会把「正文没删干净」读成正常。

    :param expected_headers: 与 GET 路径同一份响应头期望（HEAD 必须给出 GET 会发出的那份头部）
    """
    try:
        client = asyncio.run(run_probe(host, port, path, method="HEAD"))
    except Exception as probe_error:  # noqa: BLE001 - 验收探针要把任何失败原因如实报出来
        print(f"HTTP/3 HEAD 请求失败：{type(probe_error).__name__}: {probe_error}", file=sys.stderr)
        return 1

    print(f"status={client.status} headers={client.headers} 线上 {len(client.body)} 字节"
          f"（DATA 事件 {client.data_event_count} 个）")
    if not 200 <= client.status < 400:
        print(f"HEAD 没有拿到 2xx/3xx：实得 {client.status}", file=sys.stderr)
        return 1
    if client.body:
        print(f"HEAD 的响应带了正文（{len(client.body)} 字节）：RFC 9110 §9.3.2 不许", file=sys.stderr)
        return 1
    if "content-length" not in client.headers:
        print("HEAD 的响应仍要给出 GET 会发出的那份长度，实得没有 content-length", file=sys.stderr)
        return 1
    for problem in check_expected_headers(client, expected_headers or []):
        print(problem, file=sys.stderr)
        return 1
    print("HTTP/3 HEAD 验收通过")
    return 0


def main():
    if len(sys.argv) < 4:
        print("用法：h3_acceptance.py <host> <port> <path> [期望状态码] [正文片段] "
              "[--websocket <文本>] [--accept-encoding <编码>] [--head] [--stream] "
              "[--expect-header <名[=值]>]", file=sys.stderr)
        return 2

    host = sys.argv[1]
    port = int(sys.argv[2])
    path = sys.argv[3]

    # 带 --websocket 时后一个参数是帧负载的文本；这条路径验的是扩展 CONNECT 隧道，不是普通 GET
    if "--websocket" in sys.argv:
        websocket_index = sys.argv.index("--websocket")
        if websocket_index + 1 >= len(sys.argv):
            print("--websocket 后面要给出帧负载的文本", file=sys.stderr)
            return 2
        return run_websocket_mode(host, port, path, sys.argv[websocket_index + 1])

    # 流式响应的判据要单独看「分了几趟」：正文拼起来一样长，看不出是没分趟还是并成了一趟
    expect_stream = "--stream" in sys.argv

    # 一次线性扫描把开关与位置参数分开收：带值的开关连它的取值一起吃掉，剩下的才按位置读
    # （原先按「在不在列表里」逐个捞，加一个带值开关就要改两处过滤，改漏一次状态码会读到开关名）
    accept_encoding = None
    expected_headers = []
    positional_arguments = []
    index = 1
    while index < len(sys.argv):
        argument = sys.argv[index]
        if argument == "--accept-encoding":
            if index + 1 >= len(sys.argv):
                print("--accept-encoding 后面要给出编码名（本探针只解得开 gzip）", file=sys.stderr)
                return 2
            accept_encoding = sys.argv[index + 1]
            index += 2
            continue
        if argument == "--expect-header":
            if index + 1 >= len(sys.argv):
                print("--expect-header 后面要给出头名（可跟 =期望值）", file=sys.stderr)
                return 2
            expected_headers.append(sys.argv[index + 1])
            index += 2
            continue
        if argument.startswith("--"):
            # 无值开关（--head/--stream 一类）在前面已有分支判定，这里只负责别把它当成位置参数
            index += 1
            continue
        positional_arguments.append(argument)
        index += 1

    # 位置参数：期望状态码与正文片段都排在 host/port/path 之后
    expected_status = int(positional_arguments[3]) if len(positional_arguments) > 3 else 200
    expected_body = positional_arguments[4] if len(positional_arguments) > 4 else None

    # HEAD 走另一条判定（期望的响应头在这里已解析完，交给它一并核）
    if "--head" in sys.argv:
        return run_head_mode(host, port, path, expected_headers)

    try:
        client = asyncio.run(run_probe(host, port, path, accept_encoding))
    except Exception as probe_error:  # noqa: BLE001 - 验收探针要把任何失败原因如实报出来
        print(f"HTTP/3 请求失败：{type(probe_error).__name__}: {probe_error}", file=sys.stderr)
        return 1

    body_bytes = bytes(client.body)
    content_encoding = client.headers.get("content-encoding")
    if accept_encoding is not None:
        # 声明了编码却没压，或压了却没声明，都算失败：只看正文会把「压根没压」当成通过
        if content_encoding != accept_encoding:
            print(f"响应没按声明编码：请求 accept-encoding={accept_encoding}，实得 content-encoding={content_encoding!r}", file=sys.stderr)
            return 1
        if "accept-encoding" not in client.headers.get("vary", ""):
            print(f"压缩改变了表示，必须带 vary: accept-encoding，实得 {client.headers.get('vary')!r}", file=sys.stderr)
            return 1
    if content_encoding == "gzip":
        body_bytes = gzip.decompress(body_bytes)
    elif content_encoding is not None:
        print(f"响应声明了本探针解不开的编码：{content_encoding}（探针只带 gzip 解码器）", file=sys.stderr)
        return 1
    body_text = body_bytes.decode(errors="replace")
    print(f"status={client.status} headers={client.headers} 线上 {len(client.body)} 字节"
          f"（编码 {content_encoding or 'identity'}）解开后 {len(body_bytes)} 字节 body={body_text[:80]!r}")

    if client.status != expected_status:
        print(f"状态码不符：期望 {expected_status}，实得 {client.status}", file=sys.stderr)
        return 1
    # 线上正文字节数必须等于头部声明的长度（压缩时按编码后的算）：这条由探针自己核，
    # 因为 HEAD 场景已让 aioquic 免去同样的核对，两条路径不能一条严一条松
    declared_length = client.headers.get("content-length")
    if declared_length is not None and declared_length.isdigit() and int(declared_length) != len(client.body):
        print(f"content-length 与线上正文字节数不符：头部 {declared_length}，实得 {len(client.body)}", file=sys.stderr)
        return 1
    if expected_body is not None and expected_body not in body_text:
        print(f"正文里没有 {expected_body!r}", file=sys.stderr)
        return 1
    if expect_stream:
        if client.data_event_count < 2:
            print(f"流式响应没有分趟到达：只收到 {client.data_event_count} 个 DATA 事件", file=sys.stderr)
            return 1
        if "content-length" in client.headers:
            print(f"流式响应的长度此刻还不知道，不该带 content-length，实得 {client.headers['content-length']!r}", file=sys.stderr)
            return 1
    if expected_headers:
        header_problems = check_expected_headers(client, expected_headers)
        if header_problems:
            for problem in header_problems:
                print(problem, file=sys.stderr)
            return 1
    print("HTTP/3 验收通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
