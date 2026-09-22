"""h2c 压测：明文 HTTP/2（先验知识）上的协议正确性 + 单连接多路复用负载。

用法：
    python benchmarks/soak_h2c.py --port 18080 [--host 127.0.0.1] [--requests N]
                                  [--connections C] [--pipeline P] [--path /bench|/big] [--json-out <结果文件>]

服务端要求：`echo_server --h2c`（明文连接按先验知识说 h2；TLS 上的 h2 由 ALPN 协商，与本脚本无关）。
`--json-out` 把本次结果写成 JSON（结构见 benchmarks/baseline.json 的 note 字段），供 check-baseline.py 比对。
本脚本自带一个最小 h2 客户端：不依赖任何第三方库，直接拼帧——前言 + SETTINGS、请求头块用 HPACK
静态表索引（:method GET / :scheme http / :path），响应只按「DATA/HEADERS 上的 END_STREAM」判定完成，
并核对头块里出现过 :status 200 的表示（静态表索引 8）。收到的 DATA 字节会按流控规则归还（流 + 连接
两级，攒够 4 KiB 一帧），所以 `/big`（262147 字节）这类超过初始窗口 65535 的正文也能量到——
不归还窗口的客户端只能收到前 64 KiB，之后服务端就停在那条流上。

**本脚本首先是不变式用例，其次才是负载生成器**——它自己的吞吐/延迟数字受限于单线程 Python 客户端
（一次 sendall 一帧、串行解析），只适合同构建下的横向对比，不能当作引擎的 h2 性能结论。已实测的
（2026-09-21 重录，本机 Windows / MSVC / `--threads 4`，各 3 次取中位数，仅供回归对比）：
    · Release（无插桩、开 LTO，多次取中位数）：单条往返（--pipeline 1）25,542 请求/s、p50 19us；
      32 条一批（--pipeline 32，2 连接）72,346 请求/s、p50 159us。
    · Debug+ASan（2026-09-13 量，本次未重测）：单条往返约 600 请求/s、p50 ≈ 1.6ms；32 条一批 p50 ≈ 41ms。
    · 会话间离散仍然要防：本次 3 轮同一份 Release 代码只散 1.04~1.05 倍，但 09-13 那一组在较忙的会话里
      测到过 10.7k 与 56.9k——比本次低 2.4 倍与 1.27 倍。所以 benchmarks/baseline.json 的吞吐下限放在 0.6 倍：
      它抓的是量级回归，不是百分之一的抖动。
    · 单条往返的绝对数字基本由 Python 客户端的一次收发开销决定（`--threads` 1→4 无变化即为佐证）；
      32 条一批时单条延迟随排在前面的流数增长，因为服务端**先把一轮里收齐的请求全部服务完再一次性写出**
      ——多路复用省连接数，不省排队延迟。
    · 两种构建下都累计 3400+ 条请求零失败：无 GOAWAY/RST_STREAM、每条流都收到 END_STREAM 且状态 200。
    · 归还窗口这一档实测（2026-09-23，容器 ubuntu24 / GCC 13.3 / `echo_server` 的 ASan 构建）：
      `/big` 4 连接 × 20 条 = 80 条流全部收完 262147 字节（单流最多 20 帧 DATA，本端共发出 1266 帧
      WINDOW_UPDATE），零 GOAWAY/RST_STREAM、p50 9.5ms。同一份服务端上把客户端换回「不还窗口」的旧版，
      第一批就在 10s 时限上超时——这一对照就是新代码的证据。
      同一路径下 `/bench` 的热路径不受影响：正文远不到 4 KiB 的门槛，本端归还 0 帧、线上字节与旧版相同，
      5 轮中位数 12,962 对 12,975 请求/s（这个构建的轮间离散本就是 6.8k~13.6k，别看单轮）。

本脚本覆盖的**不变式**（任一违反即计入失败并以非零码退出）：
    1. 全程不得出现 GOAWAY / RST_STREAM：出现即说明服务端提前收口或拒了某条流；
    2. 每条流都必须收到带 END_STREAM 的响应（消息边界完整），且 :status 为 200；
    3. 连接必须活到最后一条请求收完（多路复用 + 流控记账在长流上不退化）；
    4. 同一路由的各条完成流，正文字节数必须一致：不一致就是某条被截断（帧边界或窗口记账出错）。
       这条判据不写死长度，因此 `/bench` 与 `/big` 共用它。它落地时立刻查出一个假绿：SETTINGS 的 ACK
       位与 END_STREAM 同为 0x1，旧解法把「服务端 ACK 了我的 SETTINGS」记成「流 0 收到完整响应」，
       于是每条连接的「少收一条响应」都会被这一条抵掉。
注意服务端默认 `maximumRequestsPerConnection = 1000`：单连接请求数超过它会被收尾 GOAWAY 收口，
那是**设计行为**而不是失败，因此脚本在超限时给出提示并按 900/连接 的建议值拆连接（见 --help）。
"""

import argparse
import json
import socket
import statistics
import struct
import sys
import threading
import time
import traceback

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

# 归还流控窗口的门槛：攒够这么多字节才发一帧 WINDOW_UPDATE（详见 FrameReader._return_consumed_window）。
# 必须明显小于对端的初始窗口（默认 65535），否则欠着的额度会把窗口吃光、把服务端卡死
WINDOW_UPDATE_THRESHOLD_BYTES = 4096


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
        """把消费掉的 DATA 字节按「流 + 连接」两级归还给服务端，攒够一挡才发。

        RFC 9113 §6.9.1：窗口只在发送方那边减少，接收方消费多少就得还多少，否则服务端迟早停在
        `SETTINGS_INITIAL_WINDOW_SIZE`（本脚本不协商这项，即默认 65535）之后不再发一个字节——
        `/big` 的 256 KiB 正文因此在 64 KiB 处收不完。归还动作贴在解帧之后、且在唯一的读入口里：
        漏一次就是自己把服务端饿死，所以不留给调用方决定。
        攒到 `WINDOW_UPDATE_THRESHOLD_BYTES` 才发一帧，是为了别把 `/bench` 这类 2 字节正文的
        热路径变成「每个响应多两帧」——那会让压测量的数字掺进客户端自己的开销。
        只要这一挡小于对端初值（4096 < 65535），欠着的额度就吃不光窗口，不会自己把服务端卡住。
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

            # END_STREAM 只在 DATA 与 HEADERS 上有定义（§6.1），且流号 0 不属于任何请求：
            # SETTINGS 的 ACK 位与 END_STREAM 同为 0x1，按标志位无脑记账会把「服务端 ACK 了我的 SETTINGS」
            # 记成「流 0 收到完整响应」，从而让「少收一条响应」这条不变式少算一条（实测如此）
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
            # 异常原先逃出线程，results[index] 就停在 None，汇总只剩一行「未知错误」，
            # 分不清是解帧、发帧还是超时——栈留在结果里，让失败结论能直接定位
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
        # 同一路由的正文是固定内容：完成流之间的字节数不一致，就是某条被截断（帧边界或窗口记账出错）。
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
        p50Milliseconds = statistics.median(all_latencies)
        p95Milliseconds = all_latencies[int(len(all_latencies) * 0.95)]
        print(f"  延迟 p50 {p50Milliseconds:.2f}ms，p95 {p95Milliseconds:.2f}ms，"
              f"max {all_latencies[-1]:.2f}ms（样本 {len(all_latencies)}）")
        measurement["p50Microseconds"] = p50Milliseconds * 1000.0
        measurement["p95Microseconds"] = p95Milliseconds * 1000.0
        measurement["maximumMicroseconds"] = all_latencies[-1] * 1000.0

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
