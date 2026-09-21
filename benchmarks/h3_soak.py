#!/usr/bin/env python3
"""HTTP/3 的进程外压测：用 aioquic 这个独立实现向本框架的 h3 服务端发负载。

为什么要有这一条：`soak.py` 走的是裸 socket，只覆盖 h1/h2c；h3 需要 QUIC 客户端，而框架本身
不带 h3 客户端，所以这条传输面此前只有「一次一条请求」的验收探针（`scripts/h3_acceptance.py`），
没有并发、没有连接 churn、更没有「连接带着挂起的业务协程一起没掉」这种收口场景。本脚本把这三段
补上：

    阶段一  并发连接 × 每条若干 GET：核对状态码与正文，给出 p50/p95/max 与 rps
            （对端是 Python 的 aioquic，这几位数读的是「客户端 + 服务端」一整圈，只用来做
            量级对照与失败检测，不当作服务端延迟基准）
    阶段二  压缩协商：带 accept-encoding 的 GET（服务端要 `--compress`），按响应头解回来核对
    阶段三  半开收口：建好 WebSocket 隧道、收到回显后客户端停手（按客户端空闲上限主动收口），
            让服务端在业务协程还挂在 receive() 上时把连接与会话一起收掉；随后再跑一轮阶段一的
            子集，要求服务照常应答、句柄与私有内存不随批次增长

用法（服务端由调用方起，与 soak.py 同一口径）：
    echo_server --host 127.0.0.1 --port 18443 --https --h3 --cert cert.pem --key key.pem
    python3 benchmarks/h3_soak.py --host 127.0.0.1 --port 18443 --pid <服务进程号>

资源漂移怎么读（本机 2026-09-21 实测，同一服务进程连跑多轮）：首轮那点正向漂移是暖机不是泄漏——
句柄在带压缩的进程里第一轮 177→185、随后各轮 185→185 与另一进程四轮 177→177；工作集只在开
`--compress` 时抬升（前三轮 5.9→6.6→6.9 MiB 后不再涨，即每条干活的线程懒建自己的压缩上下文，
上限就是线程数），`--skip-compression` 的四轮一直停在 4.2~4.9 MiB。要看的是同一进程内逐轮是否还在涨。

退出码 0 表示三段全过；任一段有失败即非 0。依赖：pip install aioquic
"""

import argparse
import asyncio
import gzip
import sys
import time
from pathlib import Path

# 复用同仓的两处现成件：soak.py 的统计与资源采样，h3_acceptance.py 的 QUIC 客户端配置与帧构造
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

from aioquic.asyncio.client import connect  # noqa: E402
from aioquic.asyncio.protocol import QuicConnectionProtocol  # noqa: E402
from aioquic.h3.connection import H3Connection  # noqa: E402
from aioquic.h3.events import DataReceived, HeadersReceived  # noqa: E402

from h3_acceptance import (  # noqa: E402
    HANDSHAKE_TIMEOUT_SECONDS,
    SETTINGS_SETTLE_SECONDS,
    make_masked_text_frame,
    make_quic_configuration,
)
from soak import ServerMonitor, Statistics  # noqa: E402

# 一条流上等到响应的上限：压测里这比握手更宽，超时即算失败
PER_REQUEST_TIMEOUT_SECONDS = 10.0

# 阶段三里让客户端自己按这个空闲上限收口：服务端默认 idleTimeout 是 30 秒，
# 靠它收要白等半分钟；客户端主动发 CONNECTION_CLOSE 走的是「对端收口」那一支，当场就到
CLIENT_IDLE_TIMEOUT_SECONDS = 2.0

# 服务端「承载连接已收口，本会话 N 条没答完的流被丢弃并叫醒业务协程」那条日志的 ASCII 之外的一段：
# 给了 --server-log 就用它核对静默收口那一支真的走到了
ABANDON_LOG_MARKER = "承载连接已收口"


class Http3LoadConnection(QuicConnectionProtocol):
    """一条 h3 客户端连接：可以在多条双向流上并发发请求，按流号把响应收全。"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._http = H3Connection(self._quic)
        self._pendingStreams = {}

    def openRequest(self, authority, path, acceptEncoding=None):
        """开一条新流发 GET，返回流号（调用方据此等响应）。"""
        headers = [
            (b":method", b"GET"),
            (b":scheme", b"https"),
            (b":authority", authority.encode()),
            (b":path", path.encode()),
        ]
        if acceptEncoding is not None:
            headers.append((b"accept-encoding", acceptEncoding.encode()))
        streamId = self._quic.get_next_available_stream_id()
        self._pendingStreams[streamId] = {"status": None, "headers": {}, "body": bytearray(), "done": asyncio.Event()}
        self._http.send_headers(stream_id=streamId, headers=headers, end_stream=True)
        self.transmit()
        return streamId

    def openTunnel(self, authority, path, frames):
        """发扩展 CONNECT（RFC 9220）并在同一条流上跟上 WebSocket 帧；这条流不收尾。"""
        streamId = self._quic.get_next_available_stream_id()
        self._pendingStreams[streamId] = {"status": None, "headers": {}, "body": bytearray(), "done": asyncio.Event()}
        self._http.send_headers(
            stream_id=streamId,
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
            self._http.send_data(stream_id=streamId, data=frame, end_stream=False)
        self.transmit()
        return streamId

    def stream(self, streamId):
        return self._pendingStreams[streamId]

    def quic_event_received(self, event):
        for httpEvent in self._http.handle_event(event):
            if not isinstance(httpEvent, (HeadersReceived, DataReceived)):
                continue
            record = self._pendingStreams.get(httpEvent.stream_id)
            if record is None:
                continue
            if isinstance(httpEvent, HeadersReceived):
                for name, value in httpEvent.headers:
                    if name == b":status":
                        record["status"] = int(value)
                    else:
                        record["headers"][name.decode()] = value.decode()
                if getattr(httpEvent, "stream_ended", False):
                    record["done"].set()
            else:
                record["body"] += httpEvent.data
                if getattr(httpEvent, "stream_ended", False):
                    record["done"].set()


async def runRequestsOnConnection(host, port, requestsPerConnection, stats, path, expectedFragment, acceptEncoding=None):
    """在一条连接上顺序发 requestsPerConnection 个请求，逐个核对并按请求计时。"""
    configuration = make_quic_configuration()
    async with connect(host, port, configuration=configuration, create_protocol=Http3LoadConnection) as client:
        await asyncio.sleep(SETTINGS_SETTLE_SECONDS)
        authority = f"{host}:{port}"
        for _ in range(requestsPerConnection):
            startedAt = time.perf_counter()
            streamId = client.openRequest(authority, path, acceptEncoding)
            record = client.stream(streamId)
            try:
                await asyncio.wait_for(record["done"].wait(), timeout=PER_REQUEST_TIMEOUT_SECONDS)
            except asyncio.TimeoutError:
                stats.recordFailure("等待响应超时")
                return
            elapsed = time.perf_counter() - startedAt
            if record["status"] != 200:
                stats.recordFailure(f"状态码 {record['status']}")
                continue
            body = bytes(record["body"])
            if record["headers"].get("content-encoding") == "gzip":
                try:
                    body = gzip.decompress(body)
                except (OSError, EOFError) as decompressError:
                    stats.recordFailure(f"gzip 解不开：{type(decompressError).__name__}")
                    continue
            if expectedFragment is not None and expectedFragment not in body.decode(errors="replace"):
                stats.recordFailure("正文里没有预期片段")
                continue
            stats.ok += 1
            stats.latencies.append(elapsed)


async def runLoad(host, port, connectionCount, requestsPerConnection, stats, path, expectedFragment, acceptEncoding=None):
    """并发开 connectionCount 条连接跑负载。"""
    await asyncio.gather(
        *(
            runRequestsOnConnection(host, port, requestsPerConnection, stats, path, expectedFragment, acceptEncoding)
            for _ in range(connectionCount)
        )
    )


async def openAbandonedTunnels(host, port, tunnelCount, stats, clientIdleTimeout=None):
    """建好隧道、收到一条回显后把连接晾着：本函数不负责收尾，连接要活着回去。

    刻意不用 `async with`：连接要跨过后面的等待才轮到「服务端怎么收口」登场——
    `clientIdleTimeout` 给秒数就是「客户端自己发 CONNECTION_CLOSE」，给 None 就是「客户端彻底静默、
    等服务端的空闲上限收」。aioquic 的 `connect()` 是异步上下文管理器而不是可等待对象，
    因此显式取它的进入/退出两口。
    """
    authority = f"{host}:{port}"
    configuration = make_quic_configuration(idle_timeout_seconds=clientIdleTimeout)
    clients = []
    for _ in range(tunnelCount):
        context = connect(host, port, configuration=configuration, create_protocol=Http3LoadConnection)
        try:
            # aioquic 的 connect() 交出的就是协议对象本身（不是 (transport, protocol) 二元组）
            protocol = await asyncio.wait_for(context.__aenter__(), timeout=HANDSHAKE_TIMEOUT_SECONDS)
        except Exception as connectError:  # noqa: BLE001 - 建不上就算失败，原因照原样报
            stats.recordFailure(f"隧道连接失败：{type(connectError).__name__}: {connectError}")
            continue
        clients.append((context, protocol))
        await asyncio.sleep(SETTINGS_SETTLE_SECONDS)
        frame = make_masked_text_frame("ping")
        streamId = protocol.openTunnel(authority, "/ws", [frame])
        record = protocol.stream(streamId)
        # 隧道不会收尾（没有 END_STREAM），因此按「状态码到了 2xx 且至少回来一帧回显」判建成，
        # 而不是等 done 事件——等不到就会一条白挂十秒
        built = False
        deadline = time.perf_counter() + HANDSHAKE_TIMEOUT_SECONDS
        while time.perf_counter() < deadline:
            status = record["status"]
            if status is not None and not 200 <= status < 300:
                break
            if status is not None and record["body"]:
                built = True
                break
            await asyncio.sleep(0.05)
        if not built:
            stats.recordFailure(f"隧道没建成或答了非 2xx：状态 {record['status']}、回显 {len(record['body'])} 字节")
            continue
        stats.ok += 1
    return clients


def describeDrift(monitor):
    """把资源采样的「起 → 止」漂移说成人话；采不到就直说。"""
    if not monitor.available or len(monitor.samples) < 2:
        return "资源采样不可用或样本不足"
    firstHandles, firstPrivate, _ = monitor.samples[0]
    lastHandles, lastPrivate, _ = monitor.samples[-1]
    peakPrivate = max(sample[1] for sample in monitor.samples)
    return (
        f"{monitor.handleLabel} {firstHandles}→{lastHandles}（漂移 {lastHandles - firstHandles}），"
        f"{monitor.memoryLabel} 峰值 {peakPrivate / 1024 / 1024:.1f} MiB、末值 {lastPrivate / 1024 / 1024:.1f} MiB"
    )


def main():
    parser = argparse.ArgumentParser(description="HTTP/3 进程外压测（对端是 aioquic）")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18443)
    parser.add_argument("--pid", type=int, default=0, help="服务进程号；给了才采样句柄与内存")
    parser.add_argument("--connections", type=int, default=24, help="阶段一并发的连接数")
    parser.add_argument("--requests-per-connection", type=int, default=20, help="阶段一每条连接的请求数")
    parser.add_argument("--tunnels", type=int, default=16, help="阶段三晾着的隧道连接数（一半走客户端收口、一半静默）")
    parser.add_argument("--reap-wait", type=float, default=6.0, help="阶段三 a 等客户端自己收口的秒数")
    parser.add_argument("--idle-reap-seconds", type=float, default=36.0,
                        help="阶段三 b 等服务端按 idleTimeout（默认 30 秒）收口的秒数；机器慢就加大")
    parser.add_argument("--skip-idle-reap", action="store_true",
                        help="跳过阶段三 b（快速冒烟用；它才是 abandonPendingStreams 那一支）")
    parser.add_argument("--server-log", default="", help="服务端日志文件；给了就拿收口日志核对阶段三 b 真的走到")
    parser.add_argument("--path", default="/bench")
    parser.add_argument("--expect", default="OK", help="正文里应出现的片段（/bench 的正文就是 OK）；给空串则只核状态码")
    parser.add_argument("--skip-compression", action="store_true", help="服务端没起 --compress 时跳过阶段二")
    arguments = parser.parse_args()

    monitor = ServerMonitor(arguments.pid)
    monitor.start()

    loadStats = Statistics("h3-负载")
    compressStats = Statistics("h3-压缩")
    closeTunnelStats = Statistics("h3-隧道-客户端收口")
    silentTunnelStats = Statistics("h3-隧道-服务端空闲收口")
    recheckStats = Statistics("h3-收口后复查")
    exitCode = 0

    try:
        asyncio.run(runLoad(arguments.host, arguments.port, arguments.connections, arguments.requests_per_connection,
                            loadStats, arguments.path, arguments.expect))
        loadStats.report()

        if not arguments.skip_compression:
            # 大正文那一档才压得动：/big 是 256 KiB 的伪随机词表，压完约三万来字节
            asyncio.run(runLoad(arguments.host, arguments.port, 2, 3, compressStats, "/big", "scheduler",
                                acceptEncoding="gzip"))
            if compressStats.errors:
                compressStats.report()
                exitCode = 1
            else:
                print(f"[{compressStats.name}] 成功 {compressStats.ok}，失败 0（gzip 往返逐字节解得开）")

        asyncio.run(runAbandonedTunnelPhase(arguments, closeTunnelStats, silentTunnelStats, recheckStats))
        closeTunnelStats.report()
        silentTunnelStats.report()
        recheckStats.report()

        if loadStats.errors or compressStats.errors or closeTunnelStats.errors or silentTunnelStats.errors or recheckStats.errors:
            exitCode = 1
    finally:
        monitor.stop()
        print(describeDrift(monitor))

    if exitCode == 0:
        print("HTTP/3 压测通过")
    else:
        print("HTTP/3 压测失败", file=sys.stderr)
    return exitCode


def countAbandonMarkersInLog(logPath):
    """数服务端日志里「本端收口了没答完的流」那条例据出现了几次；没给路径返回 None。

    日志是被重定向出来的，编码可能是 UTF-8 也可能是 GBK：先按 UTF-8 找，一个都没找到才换 GBK，
    免得两种解码各命中一次把条数翻成两倍。
    """
    if not logPath:
        return None
    try:
        raw = Path(logPath).read_bytes()
    except OSError as readError:
        print(f"读不到服务端日志 {logPath}：{readError}", file=sys.stderr)
        return None
    utf8Hits = raw.decode("utf-8", errors="ignore").count(ABANDON_LOG_MARKER)
    if utf8Hits:
        return utf8Hits
    return raw.decode("gbk", errors="ignore").count(ABANDON_LOG_MARKER)


async def runAbandonedTunnelPhase(arguments, closeStats, silentStats, recheckStats):
    """阶段三：两种「连接带着挂起的业务协程一起没掉」的收口形态，各要服务照常应答。

    3a 是客户端自己发 CONNECTION_CLOSE（快，2 秒就到）；3b 是客户端彻底静默，等服务端的
    `idleTimeout`（默认 30 秒）把连接收掉——3b 才是 `abandonPendingStreams` 那一支的正主。
    """
    closeCount = arguments.tunnels // 2
    silentCount = arguments.tunnels - closeCount

    closeClients = await openAbandonedTunnels(arguments.host, arguments.port, closeCount, closeStats,
                                             clientIdleTimeout=CLIENT_IDLE_TIMEOUT_SECONDS)
    print(f"已晾着 {len(closeClients)} 条隧道连接（客户端 {CLIENT_IDLE_TIMEOUT_SECONDS:.0f} 秒空闲上限），等 {arguments.reap_wait:.1f} 秒")
    await asyncio.sleep(arguments.reap_wait)
    for context, _ in closeClients:
        await context.__aexit__(None, None, None)

    silentClients = []
    markersBeforeSilentPhase = countAbandonMarkersInLog(arguments.server_log)
    if not arguments.skip_idle_reap:
        silentClients = await openAbandonedTunnels(arguments.host, arguments.port, silentCount, silentStats,
                                                  clientIdleTimeout=None)
        print(f"已晾着 {len(silentClients)} 条静默隧道连接，等 {arguments.idle_reap_seconds:.0f} 秒让服务端按 idleTimeout 收口")
        await asyncio.sleep(arguments.idle_reap_seconds)

    # 复查用的负载：静默那批还晾着时就能跑（服务端要是在收口上卡住或漏摘，这里最先看出来）
    await runLoad(arguments.host, arguments.port, max(2, arguments.connections // 8), 5, recheckStats,
                  arguments.path, arguments.expect)
    for context, _ in silentClients:
        await context.__aexit__(None, None, None)

    if arguments.server_log:
        # 日志是累积的，因此按「静默那支持卡前后之差」判：本批每条被收口的会话各记一行，
        # 少了就说明有连接没走过 abandonPendingStreams
        markersAfterSilentPhase = countAbandonMarkersInLog(arguments.server_log)
        if markersBeforeSilentPhase is None or markersAfterSilentPhase is None:
            silentStats.recordFailure("读不到服务端日志，无法核对静默收口那支")
        else:
            newMarkers = markersAfterSilentPhase - markersBeforeSilentPhase
            print(f"这一段里服务端新增 {newMarkers} 条「{ABANDON_LOG_MARKER}」，静默连接 {len(silentClients)} 条")
            if not arguments.skip_idle_reap and newMarkers < len(silentClients):
                silentStats.recordFailure(
                        f"只有 {newMarkers}/{len(silentClients)} 条静默连接走过收口唤醒，"
                        f"要么没按时收口（可加大 --idle-reap-seconds），要么这一支没被走到")


if __name__ == "__main__":
    sys.exit(main())
