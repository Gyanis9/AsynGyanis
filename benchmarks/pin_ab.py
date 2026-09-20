#!/usr/bin/env python3
"""绑核 A/B：在负载下统计每条工作循环线程的跨核迁移次数，用来判 --pin-threads 值不值。

用法：pin_ab.py <echo_server 路径> <端口> <线程数> <绑核 0/1> <负载秒数> <并发连接数>
输出：单行 CSV —— pinned,threads,migrations,requests

口径与坑（都是实测出来的，别绕过）：
  · 迁移数按 /proc/<pid>/task/<tid>/stat 的 processor 字段（第 39 个，跳过含空格的 comm 之后
    的下标 36）逐次采样比对得到，4 ms 一轮；小于该间隔发生的来回迁移看不见面，
    所以这是**下界**，只适合两组之间做相对比较。
  · requests 只用于确认两侧都真的跑到了负载，**不能当吞吐看**：Python 客户端受 GIL 限制，
    实测每条连接稳定在 ~1000 请求/6 秒，服务端远未饱和。要比吞吐得换成多进程客户端
    （benchmarks/soak.py 那条路子）。
  · 采样必须落在负载窗口之内：先起负载再在同一时刻记 deadline，采样跑到 deadline 就停。
"""
import os
import socket
import subprocess
import sys
import threading
import time

REQUEST = (b"GET /bench HTTP/1.1\r\nHost: localhost\r\nUser-Agent: pin-ab\r\n"
           b"Accept: */*\r\n\r\n")


def sampleProcessors(pid):
    """读每条线程当前所在的 CPU 编号，返回 {tid: processor}"""
    processors = {}
    taskDirectory = f"/proc/{pid}/task"
    try:
        threadIds = os.listdir(taskDirectory)
    except OSError:
        return processors
    for threadId in threadIds:
        try:
            with open(f"{taskDirectory}/{threadId}/stat") as handle:
                # comm 里可以带空格与括号，只能从最后一个 ") " 之后开始切字段
                fields = handle.read().rsplit(") ", 1)[1].split()
            processors[threadId] = int(fields[36])
        except (OSError, IndexError, ValueError):
            continue
    return processors


def countContentLength(headerBlock, body):
    """按 Content-Length 判断一条响应是否收完（响应拆分式的拼接不能信）"""
    declaredLength = None
    for line in headerBlock.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            declaredLength = int(line.split(b":")[1])
    return declaredLength is not None and len(body) >= declaredLength


class LoadConnection(threading.Thread):
    """一条 keep-alive 连接上的请求循环：请求-等响应-再请求"""

    def __init__(self, port, deadline):
        super().__init__(daemon=True)
        self.port = port
        self.deadline = deadline
        self.requestCount = 0

    def run(self):
        try:
            connection = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        except OSError as error:
            sys.stderr.write(f"连不上被测服务：{type(error).__name__} {error}\n")
            return
        pending = b""
        try:
            while time.time() < self.deadline:
                connection.sendall(REQUEST)
                while True:
                    received = connection.recv(65536)
                    if not received:
                        raise ConnectionError("对端在响应写完之前关闭")
                    pending += received
                    headerBlock, separator, body = pending.partition(b"\r\n\r\n")
                    if separator and countContentLength(headerBlock, body):
                        pending = b""
                        break
                self.requestCount += 1
        except (OSError, ValueError) as error:
            sys.stderr.write(f"负载连接结束：{type(error).__name__} {error}\n")
        finally:
            connection.close()


def main(serverPath, port, threadCount, isPinned, loadSeconds, connectionCount):
    arguments = [serverPath, "--port", str(port), "--threads", str(threadCount)]
    if isPinned:
        arguments.append("--pin-threads")
    server = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    try:
        # 等端口就绪：绑核与否都要用同一个判据，不能让某一侧多等或少等
        readyDeadline = time.time() + 15
        while time.time() < readyDeadline:
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("被测服务在 15 秒内没有监听")

        deadline = time.time() + loadSeconds
        connections = [LoadConnection(port, deadline) for _ in range(connectionCount)]
        for connection in connections:
            connection.start()

        # 采样窗口与负载窗口取同一个终点：先取一次基线，之后每轮和上一轮比
        previousProcessors = sampleProcessors(server.pid)
        migrations = 0
        while time.time() < deadline:
            time.sleep(0.004)
            currentProcessors = sampleProcessors(server.pid)
            for threadId, processor in currentProcessors.items():
                if threadId in previousProcessors and previousProcessors[threadId] != processor:
                    migrations += 1
            previousProcessors = currentProcessors

        for connection in connections:
            connection.join(timeout=10)
        requests = sum(connection.requestCount for connection in connections)
        print(f"{int(isPinned)},{threadCount},{migrations},{requests}")
    finally:
        server.terminate()
        server.wait(timeout=10)


if __name__ == "__main__":
    if len(sys.argv) != 7:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4] == "1",
         float(sys.argv[5]), int(sys.argv[6]))
