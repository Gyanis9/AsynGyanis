#!/usr/bin/env python3
"""QUIC 跨实现校验探针：用 aioquic 这个独立实现当对端，核对本框架 QUIC 服务端的对外行为。

为什么换成独立实现：这些判据此前由链接进来的 ngtcp2 客户端给出，而「同一棵树里互测」有两个
改不掉的毛病——① 双方共享的误解不会在这里露出来；② 实现一改，裁判跟着一起被改松（本轮就要把
服务端与流收尾改掉，正是要盯住这类倒退的时候）。改成进程外的独立实现之后，帧合不合规范由对方的
解析器判，探针只报「看到/没看到」，本仓库改不动它。

被测端是 tests/Tools/QuicProbeServer.cpp：它把内部事件打成 stdout 行（PORT/READY/CONNECTIONS/
ABORTED/DRAINING…），由 scripts/quic_cross_check.sh 起停并核对。

用法（每个场景一条，成功打印 OK <场景> 并以 0 退出）：
    quic_cross_check.py handshake-echo <port>
    quic_cross_check.py garbage-tolerant <port>
    quic_cross_check.py alpn-refusal <port>
    quic_cross_check.py stream-abort <port> [期望错误码]
    quic_cross_check.py close-on-drain <port>
    quic_cross_check.py drain-refuses-new <port>
    quic_cross_check.py per-ip-limit <port>
    quic_cross_check.py no-starve <port>
    quic_cross_check.py resume <portA> <portB>            # 第二次必须落在恢复上
    quic_cross_check.py resume-miss <portA> <portB>       # 对照组：B 没装同一份密钥
    quic_cross_check.py idle-reap <port>                  # 空闲超时后本端被收掉
    quic_cross_check.py handshake-abandon <port>          # 只发 Initial 就停：服务端要自己收掉
    quic_cross_check.py migration <port>                  # 换本地端口再说话：服务端要跟新地址并验证它

依赖：pip install aioquic
"""

import asyncio
import secrets
import socket
import ssl
import sys
import time

from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.events import (
    ConnectionTerminated,
    HandshakeCompleted,
    StreamDataReceived,
    StreamReset,
)
from aioquic.quic.logger import QuicLogger

# 每个场景的等待上限：超时即失败，不做重试——本探针是用来钉「有没有发生」，不是容错工具
WAIT_SECONDS = 8.0

# 「握手永不完成」那条要等服务端的空闲计时器走完，给的窗口比 idleTimeout 大一截即可
REAP_WAIT_SECONDS = 12.0

# 回环上的自签证书：探针不校验链（本框架的夹具证书不是公网 CA 签的）
SERVER_NAME = "127.0.0.1"


class Probe(QuicConnectionProtocol):
    """一条 QUIC 客户端连接：把 aioquic 的事件攒成探针可读的结论。"""

    def __init__(self, *args, **kwargs):
        super().__init__( *args, **kwargs)
        self.handshake_done = asyncio.Event()
        self.terminated = None
        self.reset_codes = {}
        self.stream_data = {}
        # 「这次握手是恢复来的」这个结论在 HandshakeCompleted 事件里，连接对象上没有这个属性
        self.session_resumed = False
        self._next_event_stream_id = 0
        self.tickets = []
        # 有没有见过 Retry 报文：由 datagram_received 在喂给 aioquic 之前自己认（长头且类型位为 3）
        self.saw_retry = False

    def datagram_received(self, data, addr):
        """先把这一批字节认一下形态，再原样交给 aioquic。

        Retry 不是 aioquic 抛给应用的事件，而这一格要的判据正是「服务端真的回过一条 Retry」——
        于是只能在看包的地方自己认：首字节最高位是长头，其下的类型位（0x30 那两位）为 3 即 Retry。
        """
        if len(data) >= 1 and (data[0] & 0x80) and ((data[0] >> 4) & 0x03) == 0x03:
            self.saw_retry = True
        super().datagram_received(data, addr)

    def quic_event_received(self, event):
        if isinstance(event, HandshakeCompleted):
            self.session_resumed = bool(getattr(event, "session_resumed", False))
            self.handshake_done.set()
        elif isinstance(event, ConnectionTerminated):
            self.terminated = event
            self.handshake_done.set()  # 让等待方醒来去判「以什么收场」
        elif isinstance(event, StreamReset):
            self.reset_codes[event.stream_id] = event.error_code
        elif isinstance(event, StreamDataReceived):
            self.stream_data.setdefault(event.stream_id, bytearray())
            self.stream_data[event.stream_id] += event.data
            if event.end_stream:
                self.stream_data[f"closed:{event.stream_id}"] = True

    async def wait_handshake(self):
        await asyncio.wait_for(self.handshake_done.wait(), WAIT_SECONDS)

    def send_request(self, payload: bytes) -> int:
        """在一条新的双向流上发一段正文并收尾，返回流号。"""
        stream_id = self._quic.get_next_available_stream_id()
        self._quic.send_stream_data(stream_id, payload, end_stream=True)
        self.transmit()
        return stream_id

    async def wait_stream_closed(self, stream_id: int):
        async def poll():
            while f"closed:{stream_id}" not in self.stream_data:
                await asyncio.sleep(0.02)
        await asyncio.wait_for(poll(), WAIT_SECONDS)

    async def wait_reset(self, stream_id: int) -> int:
        async def poll():
            while stream_id not in self.reset_codes:
                await asyncio.sleep(0.02)
        await asyncio.wait_for(poll(), WAIT_SECONDS)
        return self.reset_codes[stream_id]

    async def wait_terminated(self):
        async def poll():
            while self.terminated is None:
                await asyncio.sleep(0.02)
        await asyncio.wait_for(poll(), WAIT_SECONDS)


def make_configuration(alpn: str = "h3") -> QuicConfiguration:
    """客户端配置：只提一个 ALPN、不校验证书，其余取 aioquic 默认。"""
    configuration = QuicConfiguration(is_client=True, alpn_protocols=[alpn], server_name=SERVER_NAME)
    configuration.verify_mode = ssl.CERT_NONE
    configuration.cadata = None
    return configuration


async def open_connection(port: int, configuration: QuicConfiguration, *, ticket_sink=None,
                          quiet_after_initial: bool = False) -> Probe:
    """手工起一条连接：直接构造 QuicConnection 才拿得到票据回调与恢复标志。

    aioquic 的 asyncio 客户端助手不暴露 `session_ticket_handler`，也就拿不到 NewSessionTicket；
    恢复场景要把第一趟的票据喂给第二趟，只能自己造连接对象再交给协议。
    @param ticket_sink 收到 NewSessionTicket 时被调用一次，用来把票据带到下一条连接上
    @param quiet_after_initial 只发出第一批包就停手：留给服务端的空闲计时器去收
    """
    loop = asyncio.get_running_loop()
    connection = QuicConnection(
        configuration=configuration,
        session_ticket_handler=(ticket_sink if ticket_sink is not None else (lambda ticket: None)),
    )
    protocol = Probe(quic=connection)
    _transport, protocol = await loop.create_datagram_endpoint(lambda: protocol, local_addr=("127.0.0.1", 0))
    connection.connect(("127.0.0.1", port), now=time.monotonic())
    protocol.transmit()
    if quiet_after_initial:
        # 等一小段：让 Initial 真的出去，此后本端不再回应任何包
        await asyncio.sleep(0.4)
        return protocol
    await protocol.wait_handshake()
    return protocol


async def scenario_handshake_echo(port: int) -> None:
    """握手完成 + 一条流上的回显逐字对上：链路与流数据两端都走真回环 UDP。"""
    transport = await open_connection(port, make_configuration())
    stream_id = transport.send_request(b"ping")
    await transport.wait_stream_closed(stream_id)
    got = bytes(transport.stream_data[stream_id])
    if got != b"pong:ping":
        raise AssertionError(f"回显不是 pong:ping，拿到 {got!r}")


async def scenario_garbage_tolerant(port: int) -> None:
    """先来一包纯垃圾，再走正常握手与回显：垃圾不许把服务端口上的收循环带崩。"""
    junker = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    junker.sendto(secrets.token_bytes(1200), ("127.0.0.1", port))
    junker.close()
    await asyncio.sleep(0.2)
    await scenario_handshake_echo(port)


async def scenario_alpn_refusal(port: int) -> None:
    """ALPN 不对时不许握手完成：本端要求的 h3 之外一律拒绝（RFC 9001 §8.1）。"""
    transport = None
    try:
        transport = await open_connection(port, make_configuration("hq-test"))
    except (asyncio.TimeoutError, ConnectionError):
        return
    if transport is not None and transport.terminated is None and transport.handshake_done.is_set():
        raise AssertionError("ALPN 不匹配却握手完成了：本端该拒绝而不是将就")


async def scenario_stream_abort(port: int, expected_code: int) -> None:
    """服务端收口一条流时那份 RESET_STREAM 必须被独立实现读懂，且错误码逐位对上。"""
    transport = await open_connection(port, make_configuration())
    stream_id = transport.send_request(b"abort")
    code = await transport.wait_reset(stream_id)
    if code != expected_code:
        raise AssertionError(f"对端看到的收口错误码是 {code}，期望 {expected_code}")
    if transport.terminated is not None:
        raise AssertionError("只该收一条流，整条连接却被收掉了：" + str(transport.terminated))


async def scenario_close_on_drain(port: int) -> None:
    """drain 之后必须收到 CONNECTION_CLOSE：只静默消失等于让对端等自己的空闲超时。"""
    transport = await open_connection(port, make_configuration())
    stream_id = transport.send_request(b"ping")
    await transport.wait_stream_closed(stream_id)
    await transport.wait_terminated()


async def scenario_drain_refuses_new(port: int) -> None:
    """已处于收口态的服务端不该再让新连接握手完成。"""
    try:
        transport = await open_connection(port, make_configuration())
    except (asyncio.TimeoutError, ConnectionError):
        return
    if transport.terminated is None:
        raise AssertionError("drain 之后新连接仍握手成功：挡新连的标记没生效")


async def scenario_per_ip_limit(port: int) -> None:
    """同一来源的第二条连接该被拒：限额器要能在 QUIC 这条通道上拦住。"""
    first = await open_connection(port, make_configuration())
    if not first.handshake_done.is_set() or first.terminated is not None:
        raise AssertionError("前提不成立：第一条连接没握手成功")
    second_configuration = make_configuration()
    try:
        second = await open_connection(port, second_configuration)
    except (asyncio.TimeoutError, ConnectionError):
        return
    if second.terminated is None:
        raise AssertionError("同来源的第二条连接也握手成功了：单来源限额没作用于 QUIC")


async def scenario_no_starve(port: int) -> None:
    """一条被流控堵住的流不许饿死后来的流：先要一大块不收口的，再要一条小的。"""
    transport = await open_connection(port, make_configuration())
    blocked_stream = transport.send_request(b"block")
    small_stream = transport.send_request(b"other")
    await transport.wait_stream_closed(small_stream)
    got = bytes(transport.stream_data[small_stream])
    if got != b"pong:other":
        raise AssertionError(f"后来的流没拿到应得的回显：{got!r}")
    if f"closed:{blocked_stream}" in transport.stream_data:
        raise AssertionError("大块那条流不该已经收尾（探针靠它保持住发送窗口）")


async def scenario_resume(port_a: int, port_b: int, expect_resumed: bool) -> None:
    """两次连接落在两台实例上：装了同一份票据密钥的第二次必须是恢复，对照组必须不是。"""
    holder = []
    first = await open_connection(port_a, make_configuration(), ticket_sink=holder.append)
    stream_id = first.send_request(b"ping")
    await first.wait_stream_closed(stream_id)
    if not holder:
        raise AssertionError("第一趟没拿到会话票据：服务端没发 NewSessionTicket，恢复无从谈起")

    second_configuration = make_configuration()
    second_configuration.session_ticket = holder[0]
    second = await open_connection(port_b, second_configuration)
    stream_id = second.send_request(b"ping")
    await second.wait_stream_closed(stream_id)
    resumed = second.session_resumed
    if resumed != expect_resumed:
        raise AssertionError(f"第二台实例的恢复标志 = {resumed}，期望 {expect_resumed}")


async def scenario_idle_reap(port: int) -> None:
    """握手完成后一直不说话：到点本端要看到连接被收掉（空闲超时双向生效）。"""
    transport = await open_connection(port, make_configuration())
    await transport.wait_terminated()


async def scenario_handshake_abandon(port: int) -> None:
    """只发 Initial 就停：服务端不许把这条半开连接一直挂在表里（对照服务端日志的 CONNECTIONS）。"""
    await open_connection(port, make_configuration(), quiet_after_initial=True)
    await asyncio.sleep(REAP_WAIT_SECONDS)


async def scenario_retry_handshake(port: int) -> None:
    """服务端开了地址验证：本端必须先收到一条 Retry，带令牌重来才握上手、回上显。

    「收到过 Retry」由客户端自己在原始字节上认（长头且类型位为 3），不看 aioquic 的内部状态。
    aioquic 会核 Retry 的完整性标签——标签算错就直接丢掉这条报文，握手就会停在这里，
    因此「握上了」本身就是对 §17.2.5 与 RFC 9001 §5.8 那套算法的跨实现认可。
    """
    transport = await open_connection(port, make_configuration())
    if not transport.saw_retry:
        raise AssertionError("握手成了但没见过 Retry：服务端的地址验证没生效，或压根没回令牌")
    stream_id = transport.send_request(b"ping")
    await transport.wait_stream_closed(stream_id)
    got = bytes(transport.stream_data[stream_id])
    if got != b"pong:ping":
        raise AssertionError(f"走过 Retry 之后的回显不是 pong:ping，拿到 {got!r}")


async def scenario_migration(port: int) -> None:
    """连接迁移：客户端换一个本地端口继续说话，服务端必须跟过去并在新地址上验证本端。

    asyncio 那层一条连接绑一个传输，做不出「换源端口」；这里直接用 aioquic 的同步连接对象
    （它不认套接字，收发由探针自己管），两个本地套接字分别当迁移前后的腿。三条判据：
    ① 握手在第一个端口上完成；② 从第二个端口发的请求，回显必须落在第二个端口上——服务端跟了
    新地址（RFC 9000 §9.3 的 MUST）；③ 服务端的帧日志里要出现它发来的 PATH_CHALLENGE 与本端
    回的 PATH_RESPONSE（§8.2.1/§8.2.2；日志是 aioquic 自己的 qlog，不看服务端内部状态）。
    """
    configuration = make_configuration()
    quic_logger = QuicLogger()
    configuration.quic_logger = quic_logger
    connection = QuicConnection(configuration=configuration)
    loop = asyncio.get_running_loop()

    def make_socket() -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.setblocking(False)
        return sock

    staying = make_socket()
    migrating = make_socket()
    events: list = []

    async def pump_until(sock: socket.socket, predicate, timeout: float = WAIT_SECONDS) -> bool:
        """把待发从给定套接字发出去、收一条交给 aioquic，直到 predicate 成立或超时。"""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            now = time.monotonic()
            for payload, target in connection.datagrams_to_send(now=now):
                sock.sendto(payload, target)
            while (event := connection.next_event()) is not None:
                events.append(event)
            if predicate():
                return True
            timer = connection.get_timer()
            wait = 0.05 if timer is None else min(0.05, max(0.005, timer - time.monotonic()))
            try:
                payload, source = await asyncio.wait_for(loop.sock_recvfrom(sock, 65536), timeout=wait)
            except (asyncio.TimeoutError, TimeoutError):
                continue
            connection.receive_datagram(payload, source, now=time.monotonic())
        return False

    try:
        connection.connect(("127.0.0.1", port), now=time.monotonic())
        if not await pump_until(staying, lambda: any(isinstance(event, HandshakeCompleted) for event in events)):
            raise AssertionError("第一个本地端口上握手没完成")
        if any(isinstance(event, ConnectionTerminated) for event in events):
            raise AssertionError("握手之后连接就被收掉了")

        # ② 换到第二个本地端口发一条请求：回显必须落在同一侧（服务端跟了新地址）
        stream_id = connection.get_next_available_stream_id()
        connection.send_stream_data(stream_id, b"ping", end_stream=True)

        def echoed() -> bool:
            return any(isinstance(event, StreamDataReceived) and event.stream_id == stream_id and event.data == b"pong:ping" for event in events)

        if not await pump_until(migrating, echoed):
            raise AssertionError("从新端口发的请求没在新端口上收到回显：服务端没跟着新地址走（§9.3）")

        def frame_types(event_suffix: str) -> list:
            types = []
            for trace in quic_logger.to_dict()["traces"]:
                for event in trace["events"]:
                    if not event["name"].endswith(event_suffix):
                        continue
                    for frame in event["data"].get("frames") or []:
                        types.append(frame.get("frame_type"))
            return types

        if frame_types(":packet_received").count("path_challenge") < 1:
            raise AssertionError("服务端没在新地址上发起路径验证（§9.3 的 MUST initiate path validation）")
        if frame_types(":packet_sent").count("path_response") < 1:
            raise AssertionError("服务端的 PATH_CHALLENGE 没被答上：本端没回过 PATH_RESPONSE（§8.2.2）")
        if any(isinstance(event, ConnectionTerminated) for event in events):
            raise AssertionError("迁移之后连接被收掉了：" + str(next(event for event in events if isinstance(event, ConnectionTerminated))))
    finally:
        staying.close()
        migrating.close()


SCENARIOS = {
    "handshake-echo": lambda rest: scenario_handshake_echo(int(rest[0])),
    "garbage-tolerant": lambda rest: scenario_garbage_tolerant(int(rest[0])),
    "alpn-refusal": lambda rest: scenario_alpn_refusal(int(rest[0])),
    "stream-abort": lambda rest: scenario_stream_abort(int(rest[0]), int(rest[1], 0) if len(rest) > 1 else 0x010B),
    "close-on-drain": lambda rest: scenario_close_on_drain(int(rest[0])),
    "drain-refuses-new": lambda rest: scenario_drain_refuses_new(int(rest[0])),
    "per-ip-limit": lambda rest: scenario_per_ip_limit(int(rest[0])),
    "no-starve": lambda rest: scenario_no_starve(int(rest[0])),
    "resume": lambda rest: scenario_resume(int(rest[0]), int(rest[1]), True),
    "resume-miss": lambda rest: scenario_resume(int(rest[0]), int(rest[1]), False),
    "idle-reap": lambda rest: scenario_idle_reap(int(rest[0])),
    "handshake-abandon": lambda rest: scenario_handshake_abandon(int(rest[0])),
    "retry-handshake": lambda rest: scenario_retry_handshake(int(rest[0])),
    "migration": lambda rest: scenario_migration(int(rest[0])),
}


def main() -> int:
    if len(sys.argv) < 3 or sys.argv[1] not in SCENARIOS:
        print(__doc__)
        print("未支持的场景：" + str(sys.argv[1:2]), file=sys.stderr)
        return 2
    body = SCENARIOS[sys.argv[1]](sys.argv[2:])
    try:
        asyncio.run(body)
    except Exception as failure:  # noqa: BLE001 - 任何异常都是「这一条判据没成立」
        print(f"FAIL {sys.argv[1]}: {type(failure).__name__}: {failure}", file=sys.stderr)
        return 1
    print(f"OK {sys.argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
