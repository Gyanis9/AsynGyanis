"""HTTP 服务器进程外压测：协议正确性 + 保持连接负载 + 短连接 churn + 空闲连接驻留，并采样句柄/内存。

用法：
    python benchmarks/soak.py --port 18080 [--host 127.0.0.1] [--pid <服务进程号>]
                              [--keepalive-rounds N] [--churn-connections N] [--idle-connections N]
                              [--json-out <结果文件>]

`--pid` 给出服务进程号才会采样句柄与内存（Windows 上用 ctypes 直接问内核，Linux 上读 /proc/<pid>，
都不需要额外工具；两侧采的量分别是 句柄数/私有字节/工作集 与 文件描述符数/私有 RSS/常驻 RSS）。
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
import os
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
    """每秒采样一次服务进程的资源占用，只看「起 → 止」的漂移，不看绝对值。

    Windows 采（句柄数, 私有字节, 工作集字节），走 psapi/kernel32；Linux 采
    （文件描述符数, 私有 RSS, 常驻 RSS），直接读 /proc/<pid>，不需要额外工具。
    """

    def __init__(self, processId: int):
        super().__init__(daemon=True)
        self.processId = processId
        self.samples = []
        self.available = False
        self.handleLabel = "句柄"
        self.memoryLabel = "工作集"
        self._stopRequested = False
        self._sampler = None
        if sys.platform == "win32":
            self._openWindowsProcess()
        else:
            # 没有 procfs（macOS/BSD）就按「无法采样」处理，让调用方照常跑完发压阶段
            if os.path.isdir(f"/proc/{self.processId}"):
                self.handleLabel = "文件描述符"
                self.memoryLabel = "常驻 RSS"
                self._sampler = self._sampleProcFs
                self.available = True

    def _openWindowsProcess(self):
        """打开进程句柄并备好两个查询函数；打不开则保持 available 为假。"""
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]

        handle = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, False, self.processId)
        if not handle:
            handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, self.processId)
        if not handle:
            return

        kernel32.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), wintypes.DWORD]
        self._kernel32 = kernel32
        self._psapi = psapi
        self._processHandle = handle
        self._sampler = self._sampleWindows
        self.available = True

    def _sampleWindows(self):
        """取一次（句柄数, 私有字节, 工作集字节）；任一查询失败返回 None。"""
        handleCount = wintypes.DWORD(0)
        if not self._kernel32.GetProcessHandleCount(self._processHandle, ctypes.byref(handleCount)):
            return None
        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(counters)
        if not self._psapi.GetProcessMemoryInfo(self._processHandle, ctypes.byref(counters), counters.cb):
            return None
        return (handleCount.value, counters.PagefileUsage, counters.WorkingSetSize)

    def _sampleProcFs(self):
        """读 /proc 取一次（fd 数, 私有 RSS, 常驻 RSS）；进程已退出或字段读不动返回 None。"""
        try:
            descriptorCount = len(os.listdir(f"/proc/{self.processId}/fd"))
            # statm 的单位是页：size resident shared text lib data dt
            fields = open(f"/proc/{self.processId}/statm", encoding="ascii").read().split()
            residentPages = int(fields[1])
            sharedPages = int(fields[2])
        except OSError:
            return None
        pageSize = os.sysconf("SC_PAGE_SIZE")
        return (descriptorCount, (residentPages - sharedPages) * pageSize, residentPages * pageSize)

    def run(self):
        if not self.available:
            return
        while not self._stopRequested:
            result = self._sampler()
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
            # 把已收的字节一起报出来：服务端回的是 HTTP/2 帧时，头部的终止串永远等不到，
            # 只说「连接关闭」会让人去查网络；看到 00 00 xx 07 才知道对端把这条连接当成 h2c
            raise ConnectionError(
                    f"连接在头部完整前关闭，已收 {len(buffer)} 字节，开头 {buffer[:16]!r}")
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
            raise ConnectionError(
                    f"连接在正文完整前关闭，声明 {length} 字节、实收 {len(buffer)} 字节")
        buffer += chunk
    return status, headers, buffer[:length], buffer[length:]


def requestOnce(connection: socket.socket, buffer: bytes, path: bytes, method: bytes = b"GET", host: str = "127.0.0.1"):
    """发一条请求并校验响应：状态 200 且 content-length 与实收正文字节数一致。

    第三个返回值 isConnectionClosing 表示服务端已声明本条连接到此为止（Connection: close）。
    调用方必须据此换新连接再发下一条：往已声明收口的连接上继续写，是对端违约后的正常失败，
    会被误记成服务端缺陷。
    """
    connection.sendall(method + b" " + path + b" HTTP/1.1\r\nHost: " + host.encode() + b"\r\n\r\n")
    status, headers, body, buffer = readResponse(connection, buffer)
    if status != 200:
        raise ValueError(f"状态码 {status}")
    declared = int(headers.get(b"content-length", b"-1"))
    if declared != len(body):
        raise ValueError(f"content-length 声明 {declared} 实收 {len(body)}")
    return body, buffer, headers.get(b"connection") == b"close"


def runProtocolChecks(host: str, port: int) -> int:
    """协议正确性：粘包连发、404、畸形请求行、超大头部、HEAD。"""
    print("== 阶段一：协议正确性 ==")
    failures = 0

    connection = socket.create_connection((host, port), timeout=5)
    _, buffer, _ = requestOnce(connection, b"", b"/bench", host=host)
    for _ in range(50):
        _, buffer, _ = requestOnce(connection, buffer, b"/bench", host=host)
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
    """保持连接负载：每线程若干连接，每条连接上串行压满 requestCount 条。返回 (失败数, 结果字典)。

    服务端按 HttpServerLimits::maximumRequestsPerConnection（默认 1000）到点收口，回完第 1000 条
    就声明 Connection: close。本阶段要把 rounds 调到上限之上（例如 1001），所以见到 close 声明就
    换一条新连接继续跑剩余请求：这既守住「不给已收口的连接再发报文」的客户端本分，也让吞吐与
    分位在任何 rounds 下都量得准。代价是实际建立的连接数会略多于 connectionCount。
    """
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
                    _, buffer, isConnectionClosing = requestOnce(connection, buffer, paths[index % len(paths)], host=host)
                except (OSError, ConnectionError, ValueError) as exception:
                    # 落在第几条决定下一步查哪里：正好是每连接最后一条 ⇒ 客户端收尾口径，
                    # 落在中间 ⇒ 服务端提前收口（真缺陷）。不记下来的话这条线索只能靠猜。
                    # 打印放在锁内：多个 worker 同时失败时，半行交错会把这条证据糊掉
                    with lock:
                        statistics.recordFailure(type(exception).__name__)
                        print(f"  失败：第 {index} 条请求（本连接已成功 {succeeded} 条）报 {type(exception).__name__}: {exception}")
                    break
                with lock:
                    statistics.latencies.append(time.perf_counter() - begin)
                succeeded += 1
                if isConnectionClosing:
                    # 服务端已声明本条连接到此为止，必须换新连接再发下一条
                    connection.close()
                    try:
                        connection = socket.create_connection((host, port), timeout=10)
                        buffer = b""
                    except OSError as exception:
                        with lock:
                            statistics.recordFailure(f"reconnect:{type(exception).__name__}")
                        break
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
                body, _, _ = requestOnce(connection, b"", b"/bench", host=host)
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
    """空闲连接驻留：开一批连接不发数据，验证服务器的空闲超时与句柄回落。

    本阶段是「服务端还在不在接受连接」的判据：连接被拒、探针拿不到 200，都算失败项返回，
    而不是抛出异常把整轮压测崩在 traceback 上——那种失败恰恰是最需要被记成故障的现象。
    """
    print(f"== 阶段四：空闲连接驻留（{connectionCount} 条，静置 {holdSeconds:.0f}s）==")
    failures = 0
    connections = []
    for _ in range(connectionCount):
        try:
            connections.append(socket.create_connection((host, port), timeout=5))
        except OSError as exception:
            # 在监听却接不住连接（backlog 灌满时 Windows 直接回 WSAECONNREFUSED）说明服务端已经卡住，
            # 继续灌更多连接只会掩盖现场：记一笔失败就停手，让上面那行报出断在第几条
            failures += 1
            print(f"  建立第 {len(connections) + 1} 条连接失败：{type(exception).__name__}: {exception}")
            break
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

    # 空闲期间仍应能正常服务新请求：探针失败与空正文同样是「服务端卡住」的判据
    try:
        probe = socket.create_connection((host, port), timeout=5)
        try:
            body, _, _ = requestOnce(probe, b"", b"/bench", host=host)
        finally:
            probe.close()
    except (OSError, ConnectionError, ValueError) as exception:
        print(f"  空闲期间的并发请求失败：{type(exception).__name__}: {exception}")
        body = b""

    if body:
        print("  空闲期间的并发请求：正常")
    else:
        failures += 1
        print("  空闲期间的并发请求：异常（服务端在监听但没能完成这条请求）")
    return failures


def runSlowClientResilience(host: str, port: int, connectionCount: int, holdSeconds: float) -> int:
    """慢客户端（slowloris 式）：只发半截请求头就挂住，服务端必须照常服务其他连接。

    慢连接发一小段请求头后停住；挂住期间用一条正常连接连做 20 次请求并要求全部成功——
    这正是「慢客户端不影响正常服务」的判据。保持 holdSeconds 后统计被服务端收口的慢连接数
    （默认读超时 60s，短期保持内收口为 0 属正常，只作观测不作断言）。
    所有失败（建连失败、正常请求未完成）一律计入返回值，不抛 traceback。
    """
    print(f"== 阶段：慢客户端（{connectionCount} 条半截请求，挂住 {holdSeconds:.1f}s）==")
    failures = 0
    slowConnections = []
    for _ in range(connectionCount):
        try:
            connection = socket.create_connection((host, port), timeout=5)
            connection.sendall(b"GET /bench HTTP/1.1\r\nHost: slow\r\nX-Partial: hang")
            slowConnections.append(connection)
        except OSError as exception:
            failures += 1
            print(f"  慢连接建立失败：{type(exception).__name__}: {exception}")

    servedCount = 0
    probeFailure = None
    started = time.perf_counter()
    try:
        with socket.create_connection((host, port), timeout=5) as normal:
            buffer = b""
            for _ in range(20):
                body, buffer, _ = requestOnce(normal, buffer, b"/bench", host=host)
                servedCount += 1 if body == b"OK" else 0
    except (OSError, ConnectionError, ValueError) as exception:
        probeFailure = exception
    elapsed = time.perf_counter() - started
    if probeFailure is not None:
        print(f"  慢连接挂住期间正常请求中断：{type(probeFailure).__name__}: {probeFailure}")
    missedCount = 20 - servedCount
    failures += missedCount
    print(f"  慢连接挂住期间正常请求：成功 {servedCount}/20，用时 {elapsed * 1000:.1f} ms，"
          f"未完成 {missedCount} 条计入失败")

    time.sleep(holdSeconds)
    reclaimedCount = 0
    for connection in slowConnections:
        connection.settimeout(0.3)
        try:
            # 有返回（应答字节或 EOF）都说明服务端已对这条连接动作过
            connection.recv(128)
            reclaimedCount += 1
        except socket.timeout:
            pass  # 仍在读超时之内：属正常，不记失败
        except OSError:
            reclaimedCount += 1
        finally:
            connection.close()
    print(f"  挂住期内被服务端收口的慢连接：{reclaimedCount}/{len(slowConnections)}"
          f"（默认读超时 60s，短期内为 0 属正常）")
    return failures


def runConnectionSweep(host: str, port: int, levels) -> tuple:
    """连接数扫描：按并发档位建立 keep-alive 连接并各做一次请求，全部必须成功。

    每个档位先把该档连接全部建好、再逐条发请求，用来观察「并发连接数」维度下有没有
    建连失败或请求失败；返回值是失败项条数与各档位测量值（供 JSON 落盘比对）。
    """
    print("== 阶段：连接数扫描 " + "/".join(str(level) for level in levels) + " ==")
    failures = 0
    summary = {}
    for level in levels:
        started = time.perf_counter()
        connections = []
        levelFailures = 0
        for _ in range(level):
            try:
                connections.append(socket.create_connection((host, port), timeout=5))
            except OSError as exception:
                levelFailures += 1
                print(f"  并发 {level}：建连失败 {type(exception).__name__}: {exception}")
        for connection in connections:
            try:
                requestOnce(connection, b"", b"/bench", host=host)
            except (OSError, ConnectionError, ValueError) as exception:
                levelFailures += 1
                print(f"  并发 {level}：请求失败 {type(exception).__name__}: {exception}")
            finally:
                connection.close()
        elapsed = time.perf_counter() - started
        failures += levelFailures
        summary[f"connections-{level}"] = {
            "requested": level,
            "established": len(connections),
            "failures": levelFailures,
            "elapsedMilliseconds": round(elapsed * 1000.0, 2),
        }
        print(f"  并发 {level}：失败 {levelFailures} 条，建连+请求用时 {elapsed * 1000:.1f} ms")
    return failures, summary


def runRateLimitCheck(host: str, port: int, burstRequests: int) -> int:
    """限流触发：连发 burstRequests 条请求，必须出现 429（带 retry-after），随后恢复放行。

    前提是被测服务按小速率启动（run-soak.bat 的 ASYN_SOAK_RATE_LIMIT=1 模式会写入
    速率 2/s、容量 2 的配置）。一条 429 都没有说明限流没生效——计为失败而不是跳过：
    静默跳过会让限流回归无人发现。限流窗口过后轮询等待放行，超时同样计失败。
    """
    print(f"== 阶段：限流触发（连发 {burstRequests} 条）==")
    failures = 0
    limitedCount = 0
    acceptedCount = 0
    with socket.create_connection((host, port), timeout=5) as connection:
        buffer = b""
        for _ in range(burstRequests):
            connection.sendall(b"GET /bench HTTP/1.1\r\nHost: rate\r\n\r\n")
            status, headers, _, buffer = readResponse(connection, buffer)
            if status == 429:
                limitedCount += 1
                if b"retry-after" not in headers:
                    failures += 1
                    print("  429 响应缺少 retry-after")
            elif status == 200:
                acceptedCount += 1
            else:
                failures += 1
                print(f"  非预期状态码 {status}")

    if limitedCount == 0:
        failures += 1
        print("  一条 429 都没有：被测服务未启用限流（本阶段需以 ASYN_SOAK_RATE_LIMIT=1 启动服务端）")
    else:
        print(f"  429 {limitedCount}/{burstRequests}，放行 {acceptedCount} 条")

    deadline = time.perf_counter() + 10.0
    recovered = False
    while time.perf_counter() < deadline:
        with socket.create_connection((host, port), timeout=5) as probe:
            probe.sendall(b"GET /bench HTTP/1.1\r\nHost: rate\r\n\r\n")
            status, _, _, _ = readResponse(probe, b"")
            if status == 200:
                recovered = True
                break
        time.sleep(0.2)
    if recovered:
        print("  限流窗口过后已恢复放行")
    else:
        failures += 1
        print("  10s 内始终被限流：恢复放行未发生")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description="HTTP 服务器进程外压测")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--pid", type=int, default=0, help="服务进程号；给了才采样句柄与内存")
    parser.add_argument("--keepalive-rounds", type=int, default=200)
    parser.add_argument("--churn-connections", type=int, default=500)
    parser.add_argument("--idle-connections", type=int, default=200)
    parser.add_argument("--idle-seconds", type=float, default=5.0)
    parser.add_argument("--slow-connections", type=int, default=64,
                        help="慢客户端阶段建立的半截请求连接数（0 = 跳过）")
    parser.add_argument("--slow-hold-seconds", type=float, default=3.0, help="慢连接挂住时长（秒）")
    parser.add_argument("--connection-sweep", type=int, nargs="+", default=[1, 10, 50, 100],
                        help="连接数扫描的并发档位（空格分隔，如 --connection-sweep 1 10 50；cmd 会把逗号当分隔符）")
    parser.add_argument("--rate-limit-burst", type=int, default=0,
                        help="限流阶段的连发条数；0 = 跳过（该阶段要求服务端已启用限流）")
    parser.add_argument("--skip-load-stages", action="store_true",
                        help="只跑限流阶段：限流开启时负载阶段（协议检查/吞吐/空闲/慢客户端）会被限流自身干扰")
    parser.add_argument("--json-out", default="", help="把本次结果写成 JSON，供 check-baseline.py 比对")
    arguments = parser.parse_args()

    monitor = ServerMonitor(arguments.pid) if arguments.pid else None
    if monitor is not None:
        if not monitor.available:
            print(f"警告：无法打开进程 {arguments.pid} 采样，句柄与内存监测关闭")
        monitor.start()

    failures = 0
    measurements = {}

    if not arguments.skip_load_stages:
        try:
            failures += runProtocolChecks(arguments.host, arguments.port)
            keepAliveFailures, keepAliveSummary = runKeepAliveLoad(
                    arguments.host, arguments.port, 8, 8, arguments.keepalive_rounds)
            failures += keepAliveFailures
            measurements["http1-keepalive"] = keepAliveSummary

            churnFailures, churnSummary = runChurnLoad(arguments.host, arguments.port, 8, arguments.churn_connections)
            failures += churnFailures
            measurements["http1-churn"] = churnSummary

            failures += runIdleConnections(arguments.host, arguments.port, arguments.idle_connections, arguments.idle_seconds)

            if arguments.slow_connections > 0:
                failures += runSlowClientResilience(
                        arguments.host, arguments.port, arguments.slow_connections, arguments.slow_hold_seconds)

            sweepLevels = arguments.connection_sweep
            if sweepLevels:
                sweepFailures, sweepSummary = runConnectionSweep(arguments.host, arguments.port, sweepLevels)
                failures += sweepFailures
                measurements["connection-sweep"] = sweepSummary
        except (OSError, ValueError) as probeError:
            # 探针跑不通要报成「一条失败」并给出下一步，不能让 Python 回溯把结论糊掉：
            # 最常见的原因是这个端口按 h2c 专用启动（echo_server --h2c），HTTP/1.1 探针被
            # 当成 HTTP/2 前导，收到的第一帧是 GOAWAY；h2c 那一档本来就该由 soak_h2c.py 采
            failures += 1
            print(f"  探针中断：{probeError}")
            print("  若服务端启用了 h2c，这个端口只认 HTTP/2 前导，本脚本的 HTTP/1.1 阶段量不出结果；"
                  "h2c 的一档请跑 benchmarks/soak_h2c.py")
    else:
        print("== 负载阶段已跳过（--skip-load-stages：限流开启时这些探针会被限流本身干扰）==")

    if arguments.rate_limit_burst > 0:
        failures += runRateLimitCheck(arguments.host, arguments.port, arguments.rate_limit_burst)
    else:
        print("== 阶段：限流触发（未启用：传 --rate-limit-burst N，并以启用限流的服务端配合）==")

    processSamples = {}
    if monitor is not None:
        monitor.stop()
        time.sleep(1.5)
        if monitor.samples:
            first, last = monitor.samples[0], monitor.samples[-1]
            print("== 采样（起 → 止）==")
            print(f"  {monitor.handleLabel} {first[0]} → {last[0]}，私有 {first[1] // 1024} → {last[1] // 1024} KiB，"
                  f"{monitor.memoryLabel} {first[2] // 1024} → {last[2] // 1024} KiB")
            print(f"  峰值{monitor.handleLabel} {max(sample[0] for sample in monitor.samples)}，"
                  f"峰值{monitor.memoryLabel} {max(sample[2] for sample in monitor.samples) // 1024} KiB")
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
