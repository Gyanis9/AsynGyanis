"""HTTP 服务器进程外压测：协议正确性 + 保持连接负载 + 短连接 churn + 空闲连接驻留，并采样句柄/内存。

用法：
    python benchmarks/soak.py --port 18080 [--host 127.0.0.1] [--pid <服务进程号>]
                              [--keepalive-rounds N] [--churn-connections N] [--idle-connections N]
                              [--json-out <结果文件>]

`--pid` 给出服务进程号才会采样句柄与内存（Windows 上用 ctypes 直接问内核，不需要额外工具）。
`--json-out` 把本次结果写成 JSON（结构见 benchmarks/baseline.json 的 note 字段），供 check-baseline.py 比对。

参考基线（2026-09-13，本机 Windows / MSVC / 4 工作线程，仅供回归对比，不是性能上限）：
    Release（无插桩、开 LTO，各 4 次取中位数）：保持连接 30,305 请求/s、p50 242us、p95 435us，
    短连接 1,764 连接/s；同一份代码的离散范围分别是 24.8k~30.8k 与 1.3k~2.1k，换一次会话还能差近 2 倍。
    Debug+ASan：保持连接约 7.7k 请求/s、p50 ≈ 0.97ms、p95 ≈ 1.2ms，短连接约 1k 连接/s。
    两者都是 22.4 万级请求零失败、句柄数不漂移。本脚本只发压不做构建，换构建类型请自行换可执行文件。

    注意保持连接那一路的 p50 主要不是单次服务耗时，而是排队延迟：8 线程 × 8 连接共 64 条并发连接压在
    4 个工作线程上，单条请求要排在同循环的其它连接之后（8 个客户端线程自己还在争 GIL）。所以这个数
    适合看「同一台机器上的前后变化」，不适合当单次请求延迟的绝对值。

    「门禁」的用法：改动前在同一台机器上跑出基线 JSON，改动后重跑并跑 check-baseline.py 比对——
    跨机器比较没有意义（CPU 频率、电源策略、后台负载都会盖过代码差异）。benchmarks/baseline.json
    就是这份参考基线的机器可读版本。
"""

import argparse
import ctypes
import json
import socket
import statistics
import sys
import threading
import time
from ctypes import wintypes

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    """psapi!GetProcessMemoryInfo 的输出结构。"""

    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


class ServerMonitor(threading.Thread):
    """每秒采样一次服务进程的句柄数、私有字节数与工作集。"""

    def __init__(self, processId: int):
        super().__init__(daemon=True)
        self.processId = processId
        self.samples = []
        self.available = False
        self._stopRequested = False

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]

        self._handle = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, False, processId)
        if not self._handle:
            self._handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, processId)
        if not self._handle:
            return

        kernel32.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), wintypes.DWORD]
        self._kernel32 = kernel32
        self._psapi = psapi
        self.available = True

    def sample(self):
        """取一次（句柄数, 私有字节, 工作集字节）；任一查询失败返回 None。"""
        handleCount = wintypes.DWORD(0)
        if not self._kernel32.GetProcessHandleCount(self._handle, ctypes.byref(handleCount)):
            return None
        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(counters)
        if not self._psapi.GetProcessMemoryInfo(self._handle, ctypes.byref(counters), counters.cb):
            return None
        return (handleCount.value, counters.PagefileUsage, counters.WorkingSetSize)

    def run(self):
        if not self.available:
            return
        while not self._stopRequested:
            result = self.sample()
            if result is not None:
                self.samples.append(result)
            time.sleep(1.0)

    def stop(self):
        self._stopRequested = True


class Statistics:
    """一路负载的统计：成功数、失败分类与耗时分布。"""

    def __init__(self, name: str):
        self.name = name
        self.ok = 0
        self.errors = 0
        self.errorKinds = {}
        self.latencies = []

    def recordFailure(self, kind: str):
        self.errors += 1
        self.errorKinds[kind] = self.errorKinds.get(kind, 0) + 1

    def summary(self) -> dict:
        """把本路结果压成 JSON 友好的字典（门禁比对只认其中的吞吐与分位）。"""
        result = {"ok": self.ok, "errors": self.errors, "errorKinds": dict(self.errorKinds)}
        if self.latencies:
            ordered = sorted(self.latencies)
            result["p50Microseconds"] = ordered[len(ordered) // 2] * 1e6
            result["p95Microseconds"] = ordered[int(len(ordered) * 0.95)] * 1e6
            result["maximumMicroseconds"] = ordered[-1] * 1e6
        return result

    def report(self) -> None:
        line = f"[{self.name}] 成功 {self.ok}，失败 {self.errors}"
        if self.errorKinds:
            line += f"，失败分类 {self.errorKinds}"
        jsonSummary = self.summary()
        if "p50Microseconds" in jsonSummary:
            line += (
                f"，耗时 us: p50={jsonSummary['p50Microseconds']:.0f}"
                f" p95={jsonSummary['p95Microseconds']:.0f}"
                f" max={jsonSummary['maximumMicroseconds']:.0f}"
            )
        print(line)


def readResponse(connection: socket.socket, buffer: bytes, headOnly: bool = False):
    """解析一条响应，返回 (状态码, 头部字典, 正文, 剩余缓冲)。"""
    while b"\r\n\r\n" not in buffer:
        chunk = connection.recv(65536)
        if not chunk:
            raise ConnectionError("连接在头部完整前关闭")
        buffer += chunk
    head, buffer = buffer.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1])
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(b":")
        headers[name.strip().lower()] = value.strip()
    if headOnly:
        return status, headers, b"", buffer
    length = int(headers.get(b"content-length", b"0"))
    while len(buffer) < length:
        chunk = connection.recv(65536)
        if not chunk:
            raise ConnectionError("连接在正文完整前关闭")
        buffer += chunk
    return status, headers, buffer[:length], buffer[length:]


def requestOnce(connection: socket.socket, buffer: bytes, path: bytes, method: bytes = b"GET", host: str = "127.0.0.1"):
    """发一条请求并校验响应：状态 200 且 content-length 与实收正文字节数一致。"""
    connection.sendall(method + b" " + path + b" HTTP/1.1\r\nHost: " + host.encode() + b"\r\n\r\n")
    status, headers, body, buffer = readResponse(connection, buffer)
    if status != 200:
        raise ValueError(f"状态码 {status}")
    declared = int(headers.get(b"content-length", b"-1"))
    if declared != len(body):
        raise ValueError(f"content-length 声明 {declared} 实收 {len(body)}")
    return body, buffer


def runProtocolChecks(host: str, port: int) -> int:
    """协议正确性：粘包连发、404、畸形请求行、超大头部、HEAD。"""
    print("== 阶段一：协议正确性 ==")
    failures = 0

    connection = socket.create_connection((host, port), timeout=5)
    _, buffer = requestOnce(connection, b"", b"/bench", host=host)
    for _ in range(50):
        _, buffer = requestOnce(connection, buffer, b"/bench", host=host)
    connection.close()
    print("  保持连接连发 51 条：正文与 content-length 全部一致")

    for label, requestText, expectedStatus in (
            ("未知路径", b"GET /no-such-page HTTP/1.1\r\nHost: x\r\n\r\n", 404),
            ("畸形请求行", b"GARBAGE\r\n\r\n", 400),
    ):
        probe = socket.create_connection((host, port), timeout=5)
        probe.sendall(requestText)
        status, _, _, _ = readResponse(probe, b"")
        print(f"  {label} -> {status}")
        failures += 0 if status == expectedStatus else 1
        probe.close()

    probe = socket.create_connection((host, port), timeout=5)
    probe.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nX-Huge: " + b"A" * 100000 + b"\r\n\r\n")
    status, _, _, _ = readResponse(probe, b"")
    print(f"  100 KB 单头部 -> {status}")
    failures += 0 if status == 431 else 1
    probe.close()

    probe = socket.create_connection((host, port), timeout=5)
    probe.sendall(b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n")
    status, headers, _, _ = readResponse(probe, b"", headOnly=True)
    declared = int(headers.get(b"content-length", b"-1"))
    print(f"  HEAD / -> {status}，content-length={declared}，正文按约定不下发")
    failures += 0 if status in (200, 405) else 1
    probe.close()

    return failures


def runKeepAliveLoad(host: str, port: int, threadCount: int, connectionCount: int, requestCount: int):
    """保持连接负载：每线程若干连接，每条连接上串行压满 requestCount 条。返回 (失败数, 结果字典)。"""
    print(f"== 阶段二：保持连接负载（{threadCount} 线程 × {connectionCount} 连接 × {requestCount} 请求）==")
    statistics = Statistics("keep-alive")
    lock = threading.Lock()
    paths = [b"/bench", b"/json", b"/"]

    def worker():
        for _ in range(connectionCount):
            try:
                connection = socket.create_connection((host, port), timeout=10)
            except OSError as exception:
                with lock:
                    statistics.recordFailure(f"connect:{type(exception).__name__}")
                continue
            buffer = b""
            succeeded = 0
            for index in range(requestCount):
                begin = time.perf_counter()
                try:
                    _, buffer = requestOnce(connection, buffer, paths[index % len(paths)], host=host)
                except (OSError, ConnectionError, ValueError) as exception:
                    with lock:
                        statistics.recordFailure(type(exception).__name__)
                    break
                with lock:
                    statistics.latencies.append(time.perf_counter() - begin)
                succeeded += 1
            connection.close()
            with lock:
                statistics.ok += succeeded

    begin = time.perf_counter()
    workers = [threading.Thread(target=worker) for _ in range(threadCount)]
    for thread in workers:
        thread.start()
    for thread in workers:
        thread.join()
    elapsed = time.perf_counter() - begin

    throughput = statistics.ok / elapsed if elapsed > 0 else 0.0
    print(f"  用时 {elapsed:.2f}s，吞吐 {throughput:.0f} 请求/s")
    statistics.report()
    summary = statistics.summary()
    summary["throughputPerSecond"] = throughput
    summary["throughputUnit"] = "请求/s"
    return statistics.errors, summary


def runChurnLoad(host: str, port: int, threadCount: int, connectionCount: int):
    """短连接 churn：一条请求一条连接，压 accept/close 路径。返回 (失败数, 结果字典)。"""
    print(f"== 阶段三：短连接 churn（{threadCount} 线程 × {connectionCount} 次连接）==")
    statistics = Statistics("churn")
    lock = threading.Lock()

    def worker():
        for _ in range(connectionCount):
            begin = time.perf_counter()
            try:
                connection = socket.create_connection((host, port), timeout=10)
                body, _ = requestOnce(connection, b"", b"/bench", host=host)
                connection.close()
                if not body:
                    raise ValueError("正文为空")
                with lock:
                    statistics.ok += 1
                    statistics.latencies.append(time.perf_counter() - begin)
            except (OSError, ConnectionError, ValueError) as exception:
                with lock:
                    statistics.recordFailure(type(exception).__name__)

    begin = time.perf_counter()
    workers = [threading.Thread(target=worker) for _ in range(threadCount)]
    for thread in workers:
        thread.start()
    for thread in workers:
        thread.join()
    elapsed = time.perf_counter() - begin

    throughput = statistics.ok / elapsed if elapsed > 0 else 0.0
    print(f"  用时 {elapsed:.2f}s，吞吐 {throughput:.0f} 连接/s")
    statistics.report()
    summary = statistics.summary()
    summary["throughputPerSecond"] = throughput
    summary["throughputUnit"] = "连接/s"
    return statistics.errors, summary


def runIdleConnections(host: str, port: int, connectionCount: int, holdSeconds: float) -> int:
    """空闲连接驻留：开一批连接不发数据，验证服务器的空闲超时与句柄回落。"""
    print(f"== 阶段四：空闲连接驻留（{connectionCount} 条，静置 {holdSeconds:.0f}s）==")
    connections = []
    try:
        for _ in range(connectionCount):
            connections.append(socket.create_connection((host, port), timeout=5))
    except OSError as exception:
        print(f"  建立第 {len(connections) + 1} 条连接失败：{type(exception).__name__}: {exception}")
    print(f"  已建立 {len(connections)} 条空闲连接，静置中")

    time.sleep(holdSeconds)

    closedByServer = 0
    for connection in connections:
        connection.settimeout(0.2)
        try:
            closedByServer += 1 if connection.recv(64) == b"" else 0
        except OSError:
            pass
        connection.close()
    print(f"  静置后被服务端收口 {closedByServer} 条（服务器配了空闲超时才会 > 0）")

    # 空闲期间仍应能正常服务新请求
    probe = socket.create_connection((host, port), timeout=5)
    body, _ = requestOnce(probe, b"", b"/bench", host=host)
    probe.close()
    print(f"  空闲期间的并发请求：{'正常' if body else '异常'}")
    return 0 if body else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="HTTP 服务器进程外压测")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--pid", type=int, default=0, help="服务进程号；给了才采样句柄与内存")
    parser.add_argument("--keepalive-rounds", type=int, default=200)
    parser.add_argument("--churn-connections", type=int, default=500)
    parser.add_argument("--idle-connections", type=int, default=200)
    parser.add_argument("--idle-seconds", type=float, default=5.0)
    parser.add_argument("--json-out", default="", help="把本次结果写成 JSON，供 check-baseline.py 比对")
    arguments = parser.parse_args()

    monitor = ServerMonitor(arguments.pid) if arguments.pid else None
    if monitor is not None:
        if not monitor.available:
            print(f"警告：无法打开进程 {arguments.pid} 采样，句柄与内存监测关闭")
        monitor.start()

    failures = 0
    measurements = {}

    failures += runProtocolChecks(arguments.host, arguments.port)
    keepAliveFailures, keepAliveSummary = runKeepAliveLoad(
            arguments.host, arguments.port, 8, 8, arguments.keepalive_rounds)
    failures += keepAliveFailures
    measurements["http1-keepalive"] = keepAliveSummary

    churnFailures, churnSummary = runChurnLoad(arguments.host, arguments.port, 8, arguments.churn_connections)
    failures += churnFailures
    measurements["http1-churn"] = churnSummary

    failures += runIdleConnections(arguments.host, arguments.port, arguments.idle_connections, arguments.idle_seconds)

    processSamples = {}
    if monitor is not None:
        monitor.stop()
        time.sleep(1.5)
        if monitor.samples:
            first, last = monitor.samples[0], monitor.samples[-1]
            print("== 采样（起 → 止）==")
            print(f"  句柄 {first[0]} → {last[0]}，私有 {first[1] // 1024} → {last[1] // 1024} KiB，"
                  f"工作集 {first[2] // 1024} → {last[2] // 1024} KiB")
            print(f"  峰值句柄 {max(sample[0] for sample in monitor.samples)}，"
                  f"峰值工作集 {max(sample[2] for sample in monitor.samples) // 1024} KiB")
            processSamples = {
                "handleCountFirst": first[0],
                "handleCountLast": last[0],
                "handleCountPeak": max(sample[0] for sample in monitor.samples),
                "workingSetFirstKib": first[2] // 1024,
                "workingSetLastKib": last[2] // 1024,
            }

    print(f"== 汇总：失败项 {failures} 条 ==")

    if arguments.json_out:
        document = {
            "benchmark": "http1",
            "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "target": f"{arguments.host}:{arguments.port}",
            "failures": failures,
            "measurements": measurements,
            "processSamples": processSamples,
        }
        with open(arguments.json_out, "w", encoding="utf-8") as stream:
            json.dump(document, stream, ensure_ascii=False, indent=2)
        print(f"  结果已写入 {arguments.json_out}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
