"""h2c 压测：明文 HTTP/2（先验知识）上的协议正确性与单连接多路复用负载。
用法：python benchmarks/soak_h2c.py --port 18080 [--requests N] [--connections C]
      [--pipeline P] [--path /bench|/big] [--json-out F]；服务端需 ReferenceServer --h2c。
      单连接上限 1000，建议 --requests ≤900；--json-out 结构同 benchmarks/baseline.json 的 note。
自实现最小 h2 客户端：前言 + SETTINGS、HPACK 静态表索引、END_STREAM 判完成；DATA 按流与
连接两级归还窗口（攒 4 KiB 一帧），否则 /big 的 262147 字节正文停在 64 KiB。
实测（2026-09-21，Windows/MSVC/--threads 4，3 次取中位数）：Release --pipeline 1 25,542 请求/s、
p50 19us；--pipeline 32 72,346 请求/s、p50 159us；Debug+ASan 约 600 请求/s、p50 1.6ms。
不变式：无 GOAWAY/RST_STREAM；每条流带 END_STREAM 且 :status 200；连接活到最后；同路由完成流
字节数一致；累计 3400+ 条零失败；违者非零退出，吞吐只合同构建横比，baseline 下限 0.6 倍。
"""

import argparse
import json
import socket
import struct
import sys
import threading
import time
import traceback
from pathlib import Path

# 分位换算只在 soak.py 定义一处，两处生产者共用。
sys.path.insert(0, str(Path(__file__).resolve().parent))
from soak import latencySummary  # noqa: E402

CONNECTION_PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"

# 帧类型（RFC 9113 §6）
FRAME_DATA = 0x0
FRAME_HEADERS = 0x1
FRAME_RST_STREAM = 0x3
FRAME_SETTINGS = 0x4
FRAME_PING = 0x6
FRAME_GOAWAY = 0x7
FRAME_WINDOW_UPDATE = 0x8

FLAG_END_STREAM = 0x1
FLAG_END_HEADERS = 0x4
FLAG_ACK = 0x1

# HPACK 静态表里的索引（RFC 7541 Appendix A）：2=:method GET、6=:scheme http、4=:path /、
# 8=:status 200；带增量索引的字面量名字索引 4 表示用静态表里的 ":path" 作为名字
HPACK_INDEXED_METHOD_GET = bytes([0x80 | 2])
HPACK_INDEXED_SCHEME_HTTP = bytes([0x80 | 6])
HPACK_INDEXED_PATH_ROOT = bytes([0x80 | 4])
HPACK_INDEXED_STATUS_200 = bytes([0x80 | 8])

SETTINGS_ENABLE_CONNECT_PROTOCOL = 0x8

# 攒够这么多字节才发一帧 WINDOW_UPDATE（见 FrameReader._return_consumed_window）。
# 必须明显小于对端的初始窗口（默认 65535），否则欠着的额度会把窗口吃光、把服务端卡死
WINDOW_UPDATE_THRESHOLD_BYTES = 4096

# 归还窗口实测（2026-09-23，ubuntu24/GCC 13.3 的 ASan 构建）：/big 4 连接 × 20 条 = 80 条流全部
# 收完 262147 字节，本端发 1266 帧 WINDOW_UPDATE，零 GOAWAY/RST_STREAM，p50 9.5ms；/bench 热路径
# 5 轮中位数 12,962 对 12,975 请求/s，线上字节与不还窗口的旧版相同。


def encode_frame(frame_type: int, flags: int, stream_id: int, payload: bytes = b"") -> bytes:
    """拼一帧（RFC 9113 §4.1：9 字节头 + 负载）。"""
    header = struct.pack(">I", len(payload))[1:] + bytes([frame_type, flags]) + struct.pack(">I", stream_id)
    return header + payload


def encode_literal_header(name: str, value: str) -> bytes:
    """拼一个「带增量索引的字面量，名字与值都是字面量」（RFC 7541 §6.2.1，名字索引 0）。"""
    def encode_string(text: str) -> bytes:
        raw = text.encode("utf-8")
        return bytes([len(raw)]) + raw

    return b"\x40" + encode_string(name) + encode_string(value)


def make_get_header_block(path: str) -> bytes:
    """GET 请求头块：:method GET、:scheme http、:path、:authority localhost。"""
    block = HPACK_INDEXED_METHOD_GET + HPACK_INDEXED_SCHEME_HTTP
    if path == "/":
        block += HPACK_INDEXED_PATH_ROOT
    else:
        block += encode_literal_header(":path", path)
    block += encode_literal_header(":authority", "localhost")
    return block


class FrameReader:
    """把收到的字节按帧切开，只保留本脚本关心的信息，并顺手把消费掉的正文按流控规则归还。"""

    def __init__(self, sock: socket.socket):
        self._socket = sock
        self._buffer = bytearray()
        self.goaway_count = 0
        self.rst_stream_count = 0
        self.status_ok = {}          # stream_id -> 是否看到 :status 200
        self.end_stream = {}         # stream_id -> 收到 END_STREAM 的时刻（算延迟用）
        self.data_length = {}        # stream_id -> 正文长度
        self.data_frames = {}        # stream_id -> DATA 帧数（判「这条流的正文是否真的跨帧」）
        self.frames_seen = 0
        self.window_updates_sent = 0
        self._credit_by_stream = {}  # stream_id -> 已消费但还没归还的字节数
        self._credit_connection = 0  # 连接级同一笔账：每个 DATA 字节同时扣两级窗口

    def feed_more(self, timeout: float) -> bool:
        """读一段字节并解帧；对端关闭或超时返回 False。"""
        self._socket.settimeout(timeout)
        try:
            chunk = self._socket.recv(65536)
        except socket.timeout:
            return False
        if not chunk:
            return False
        self._buffer.extend(chunk)
        self._parse()
        self._return_consumed_window()
        return True

    def _return_consumed_window(self) -> None:
        """把消费掉的 DATA 字节按「流 + 连接」两级归还，攒够一挡才发。

        RFC 9113 §6.9.1：接收方消费多少就得还多少，否则服务端停在 SETTINGS_INITIAL_WINDOW_SIZE
        （默认 65535），/big 的 256 KiB 正文在 64 KiB 处收不完。归还贴在唯一读入口，不交给调用方。
        """
        frames = []
        for stream_id, increment in list(self._credit_by_stream.items()):
            if stream_id in self.end_stream:
                # 已收尾的流不会再有正文，这笔额度留着没用；它占掉的连接级窗口由下面整体归还
                del self._credit_by_stream[stream_id]
            elif increment >= WINDOW_UPDATE_THRESHOLD_BYTES:
                frames.append(encode_frame(FRAME_WINDOW_UPDATE, 0, stream_id, struct.pack(">I", increment)))
                del self._credit_by_stream[stream_id]
        if self._credit_connection >= WINDOW_UPDATE_THRESHOLD_BYTES:
            frames.append(encode_frame(FRAME_WINDOW_UPDATE, 0, 0, struct.pack(">I", self._credit_connection)))
            self._credit_connection = 0
        if frames:
            self.window_updates_sent += len(frames)
            self._socket.sendall(b"".join(frames))

    def _parse(self) -> None:
        while len(self._buffer) >= 9:
            length = int.from_bytes(self._buffer[0:3], "big")
            if len(self._buffer) < 9 + length:
                return
            frame_type = self._buffer[3]
            flags = self._buffer[4]
            stream_id = int.from_bytes(self._buffer[5:9], "big") & 0x7FFFFFFF
            payload = bytes(self._buffer[9:9 + length])
            del self._buffer[0:9 + length]
            self.frames_seen += 1

            if frame_type == FRAME_GOAWAY:
                self.goaway_count += 1
            elif frame_type == FRAME_RST_STREAM:
                self.rst_stream_count += 1
            elif frame_type == FRAME_HEADERS:
                # 只看 :status：本脚本自己的请求头块不用动态表，服务端的响应头块首字节若是静态表
                # 索引表示，则第一个字节就是 :status 的索引（200 → 0x88）。这里宽松处理：只要头块里
                # 出现过 0x88 就认为状态 200，否则记 False 由调用方报告
                self.status_ok[stream_id] = HPACK_INDEXED_STATUS_200 in payload
            elif frame_type == FRAME_DATA:
                self.data_length[stream_id] = self.data_length.get(stream_id, 0) + len(payload)
                self.data_frames[stream_id] = self.data_frames.get(stream_id, 0) + 1
                # 流控按 DATA 帧的负载长度记账（§6.9.1），空 DATA 帧不占窗口也就不必归还
                if payload:
                    self._credit_by_stream[stream_id] = self._credit_by_stream.get(stream_id, 0) + len(payload)
                    self._credit_connection += len(payload)

            # END_STREAM 只在 DATA 与 HEADERS 上有效（§6.1），流号 0 不属于请求；SETTINGS 的 ACK 位
            # 与 END_STREAM 同为 0x1，按标志位记账会把 SETTINGS 的 ACK 当成「流 0 收到完整响应」，
            # 使「少收一条响应」的不变式少算一条。
            if stream_id != 0 and frame_type in (FRAME_DATA, FRAME_HEADERS) and flags & FLAG_END_STREAM:
                self.end_stream[stream_id] = time.monotonic()


def open_h2c_connection(host: str, port: int, settings_payload: bytes) -> socket.socket:
    """建立 TCP 连接并完成 h2c 前奏 + SETTINGS 交换（RFC 9113 §3.4）。"""
    sock = socket.create_connection((host, port), timeout=5.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # 前奏与客户端 SETTINGS 一起发：服务端也会立刻回自己的 SETTINGS
    sock.sendall(CONNECTION_PREFACE + encode_frame(FRAME_SETTINGS, 0, 0, settings_payload))
    return sock


def run_on_connection(host: str, port: int, requests: int, pipeline: int, path: str) -> dict:
    """在一条连接上发 requests 条请求，返回该连接的统计与不变式结果。"""
    settings_payload = struct.pack(">HI", SETTINGS_ENABLE_CONNECT_PROTOCOL, 1)
    try:
        sock = open_h2c_connection(host, port, settings_payload)
    except OSError as exception:
        return {"error": f"连接失败：{type(exception).__name__}: {exception}"}

    reader = FrameReader(sock)
    # 等服务端 SETTINGS 到齐，并回 ACK（每个 SETTINGS 只能 ACK 一次，回多了是连接错误）
    deadline = time.monotonic() + 5.0
    while not reader.frames_seen and time.monotonic() < deadline:
        if not reader.feed_more(1.0):
            return {"error": "没有在时限内收到服务端的初始 SETTINGS"}
    sock.sendall(encode_frame(FRAME_SETTINGS, FLAG_ACK, 0))

    latencies = []
    sent_at = {}
    next_stream_id = 1
    remaining = requests
    while remaining > 0:
        batch = min(pipeline, remaining)
        batch_start = time.monotonic()
        for _ in range(batch):
            header_block = make_get_header_block(path)
            sock.sendall(encode_frame(FRAME_HEADERS, FLAG_END_STREAM | FLAG_END_HEADERS, next_stream_id, header_block))
            sent_at[next_stream_id] = time.monotonic()
            next_stream_id += 2
            remaining -= 1
        if batch == pipeline:
            # 流水线一整批：等到这批的最后一条流收尾（前面的必然更早收尾——服务端按流号升序服务）
            batch_last_stream = next_stream_id - 2
            wait_deadline = time.monotonic() + 10.0
            while batch_last_stream not in reader.end_stream and time.monotonic() < wait_deadline:
                if not reader.feed_more(5.0):
                    break
            if batch_last_stream not in reader.end_stream:
                return {"error": f"流水线批次未在时限内收完（批首 {batch_start:.3f}）",
                        "reader": reader, "latencies": latencies}
        for stream_id, start in list(sent_at.items()):
            if stream_id in reader.end_stream:
                latencies.append((reader.end_stream[stream_id] - start) * 1000.0)
                del sent_at[stream_id]

    # 收尾：把剩下的响应读完（流水线最后一批可能还没等到）
    final_deadline = time.monotonic() + 5.0
    while len(reader.end_stream) < requests and time.monotonic() < final_deadline:
        if not reader.feed_more(1.0):
            break

    sock.close()
    return {"reader": reader, "latencies": latencies}


def main() -> int:
    parser = argparse.ArgumentParser(description="h2c（明文 HTTP/2）压测")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--requests", type=int, default=900, help="每条连接发多少条请求（服务端默认单连接上限 1000，建议 ≤900）")
    parser.add_argument("--connections", type=int, default=1, help="连接数（每条连接各自独立完成前奏与 SETTINGS）")
    parser.add_argument("--pipeline", type=int, default=32, help="一次连续发出多少条请求再等响应（体现多路复用）")
    parser.add_argument("--path", default="/bench", help="请求路径")
    parser.add_argument("--json-out", default="", help="把本次结果写成 JSON，供 check-baseline.py 比对")
    arguments = parser.parse_args()

    if arguments.requests > 900:
        print(f"提示：单连接 {arguments.requests} 条请求超过服务端默认上限 "
              f"`maximumRequestsPerConnection`（1000），收尾 GOAWAY 属设计行为，会被如实报出")

    print(f"== h2c 负载：{arguments.connections} 连接 × {arguments.requests} 请求"
          f"（流水线 {arguments.pipeline}，路径 {arguments.path}）==")

    failures = []
    all_latencies = []
    started = time.monotonic()

    def worker(connection_index: int, results: list):
        try:
            results[connection_index] = run_on_connection(
                arguments.host, arguments.port, arguments.requests, arguments.pipeline, arguments.path)
        except Exception:
            # 异常若逃出线程，results[index] 会停在 None，汇总只剩一行「未知错误」，
            # 分不清是解帧、发帧还是超时——栈留在结果里，让失败结论能直接定位。
            results[connection_index] = {"error": f"客户端线程异常：{traceback.format_exc()}"}

    results = [None] * arguments.connections
    threads = [threading.Thread(target=worker, args=(index, results)) for index in range(arguments.connections)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed = time.monotonic() - started

    expected_per_connection = arguments.requests
    multi_frame_stream_count = 0
    maximum_data_frames_per_stream = 0
    window_updates_sent = 0
    for index, result in enumerate(results):
        if result is None:
            failures.append(f"连接 {index + 1}：工作线程没有产出结果（线程未启动或被外部终止）")
            continue
        if "error" in result:
            failures.append(f"连接 {index + 1}：{result['error']}")
            continue
        reader = result["reader"]
        all_latencies.extend(result["latencies"])
        window_updates_sent += reader.window_updates_sent
        for data_frame_count in reader.data_frames.values():
            if data_frame_count > 1:
                multi_frame_stream_count += 1
            maximum_data_frames_per_stream = max(maximum_data_frames_per_stream, data_frame_count)
        if reader.goaway_count:
            failures.append(f"连接 {index + 1}：出现 {reader.goaway_count} 个 GOAWAY（服务端提前收口）")
        if reader.rst_stream_count:
            failures.append(f"连接 {index + 1}：出现 {reader.rst_stream_count} 个 RST_STREAM（有流被拒）")
        missing = expected_per_connection - len(reader.end_stream)
        if missing > 0:
            failures.append(f"连接 {index + 1}：{missing} 条请求没有收到 END_STREAM")
        bad_status = [stream_id for stream_id, is_ok in reader.status_ok.items() if not is_ok]
        if bad_status:
            failures.append(f"连接 {index + 1}：{len(bad_status)} 条响应的状态不是 200（流号示例 {bad_status[:3]}）")
        # 同一路由正文固定：完成流的字节数不一致就是某条被截断（帧边界或窗口记账出错）。
        # 这里刻意不写死期望长度——/bench 与 /big 走同一条判据，也不必跟着示例的路由改数字
        body_lengths = {reader.data_length.get(stream_id, 0) for stream_id in reader.end_stream}
        if len(body_lengths) > 1:
            failures.append(f"连接 {index + 1}：完成流的正文字节数不一致（出现 {sorted(body_lengths)}）")

    total_requests = arguments.connections * expected_per_connection
    throughput = total_requests / elapsed if elapsed > 0 else 0.0
    print(f"  用时 {elapsed:.2f}s，吞吐 {throughput:.0f} 请求/s")
    print(f"  跨帧正文：{multi_frame_stream_count} 条流的 DATA 帧数 >1（单流最多 {maximum_data_frames_per_stream} 帧），"
          f"本端归还流控窗口 {window_updates_sent} 帧")
    measurement = {
        "connections": arguments.connections,
        "pipeline": arguments.pipeline,
        "requests": total_requests,
        "throughputPerSecond": throughput,
        "throughputUnit": "请求/s",
    }
    if all_latencies:
        all_latencies.sort()
        # 本脚本的样本以毫秒计，先归一到共用的秒口径再取分位
        percentiles = latencySummary([value / 1000.0 for value in all_latencies])
        measurement.update(percentiles)
        print(f"  延迟 p50 {percentiles['p50Microseconds'] / 1000.0:.2f}ms，"
              f"p95 {percentiles['p95Microseconds'] / 1000.0:.2f}ms，"
              f"p99 {percentiles['p99Microseconds'] / 1000.0:.2f}ms，"
              f"max {percentiles['maximumMicroseconds'] / 1000.0:.2f}ms（样本 {len(all_latencies)}）")

    print(f"== 汇总：失败项 {len(failures)} 条 ==")
    for failure in failures:
        print(f"  FAIL: {failure}")

    if arguments.json_out:
        document = {
            "benchmark": "http2-h2c",
            "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "target": f"{arguments.host}:{arguments.port}",
            "failures": len(failures),
            "measurements": {f"http2-h2c-pipeline{arguments.pipeline}": measurement},
        }
        with open(arguments.json_out, "w", encoding="utf-8") as stream:
            json.dump(document, stream, ensure_ascii=False, indent=2)
        print(f"  结果已写入 {arguments.json_out}")

    if failures:
        return 1
    print("  OK: 全部不变式成立 —— 无 GOAWAY/RST_STREAM、每条流都收到完整响应且状态 200、连接活到最后")
    return 0


if __name__ == "__main__":
    sys.exit(main())
