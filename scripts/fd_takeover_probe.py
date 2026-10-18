#!/usr/bin/env python3
"""监听套接字交接与 socket activation 的进程外实测（POSIX 才有这条路）。

这里验的是「部署形态」那一格：镜像与 systemd 单元装的是 ReferenceServer，换代时监听套接字能不能
交接到新一代手里。库里那段（Core::adoptHandedOverListener 与各 adopt 构造子）有单测，示例的命令行
接线只有真起进程才测得出来——而这条路上最坏的形状不是报错，是「以为在接手、其实各绑各的」。

判据（任一条不成立就退出码非 0）：

  1  --adopt-fd 交一枚已在监听的 TCP 套接字 → 新一代在同一个端口上应答；交棒方关掉自己那一份
     之后仍应答（端口从不关闭，这就是零停机的读数）
  2  systemd 的形状：LISTEN_PID=本进程 + LISTEN_FDS=1 → 同一端口应答，不用写 --adopt-fd
  3  LISTEN_PID 是别人的 pid → 日志里必须出现「忽略 systemd 交来的」，随后按老路自己 bind 并服务
  4  strace：接管形状下不许对任何描述符 bind/listen。重复 listen 的实测症状是「端口换了、原服务
     不再应答」而不是报错，sanitizer 也不报，只能这样钉
  5  数据报那一枚：接管来的 UDP 套接字交给 QuicServer 后 h3 在同一端口上真应答——不看 alt-svc
     那种静态通告（那是路由表里写死的头，服务没起来也照发）
  6  换代：A 台服务完一条真 h3 连接后退场，B 台带同一份 --stateless-reset-key 接手同一批描述符 →
     老连接那条目的标识由 B 回一条无状态重置，尾部 16 字节逐字等于 A 当初宣告的那枚；同端口上
     新开一条 h3 连接照样 200（在途 QUIC 连接状态本身不可转移，判据就到「客户端能快速重连」为止）

第 6 条的两枚令牌都是独立裁判：一枚是线路上由服务端在传输参数里签发、被 aioquic 存下来的；另一枚
按 RFC 9000 §10.3.2 用同一份密钥在本地现算（HMAC-SHA-256 取前 16 字节），与本仓代码不同源。aioquic
1.3.0 认不出入向的无状态重置（receive_datagram 里没有拿 _peer_cid.stateless_reset_token 比对报文尾部
的分支，解不开头就丢），所以「客户端收到了」只能在包一层判，不能等它抛事件。

用法（要在带 aioquic 的解释器下跑；没有就把第 5、6 组自动跳过，其余照样钉）：

    scripts/fd_takeover_probe.py --server build/release/samples/ReferenceServer \
        --cert tests/Core/fixtures/test_localhost_cert.pem \
        --key tests/Core/fixtures/test_localhost_key.pem

Windows 上描述符不随 exec 继承（那份形状由 CoreWorker 示例经 SCM_RIGHTS 之外的通道演示），本脚本
如实报「本平台不适用」并退出 0；示例在那一侧遇到接管的描述符会当场失败而不是退化成自己 bind。
"""

import argparse
import asyncio
import hashlib
import hmac
import http.client
import os
import re
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time

# 本仓服务端签发的连接标识固定 18 字节（QuicConnection::kSourceConnectionIdLength）：短头不带长度
# 字段，造触发报文时必须按这个定长填目的标识，否则解头先失败，拿不到「无主标识」那一档
CID_LENGTH = 18
TRIGGER_DATAGRAM_BYTES = 50
RESET_WAIT_SECONDS = 3.0
HANDOVER_WAIT_SECONDS = 25.0
# 老连接要跨「停 A、起 B」这段时间还活着；实际上限仍由服务端谈定的那份说了算
CLIENT_IDLE_TIMEOUT_SECONDS = 60

FAILURES = []


def check(name, ok, detail=""):
    """一条判据一行读数：绿也要把看到的数字抄出来，否则「PASS」本身不可复核。"""
    print(("PASS  " if ok else "FAIL  ") + name + (("  | " + detail) if detail else ""), flush=True)
    if not ok:
        FAILURES.append(name)


def make_tcp_listener(port):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(64)
    return listener


def make_udp_socket(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", port))
    return sock


def pick_port():
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


class Server:
    """一台被实测的服务进程：strace 记账、日志落盘、描述符都由本脚本（监督方）递过去。"""

    def __init__(self, options, tag, port):
        self.options = options
        self.port = port
        self.trace = os.path.join(options.workspace, tag + ".strace")
        self.log = os.path.join(options.workspace, tag + ".log")
        self.process = None

    def command(self, descriptors, trace_binds):
        argv = [self.options.server, "--host", "127.0.0.1", "--port", str(self.port)]
        if trace_binds:
            # strace 是这台的父进程：只对它发信号会留下一个还在读同一枚套接字的孤儿，「这一台退了」
            # 那句判据就成假的，因此起的时候就要自成进程组（见 stop()）
            argv = ["strace", "-f", "-qq", "-e", "trace=bind,listen,execve", "-o", self.trace] + argv
        for descriptor in descriptors:
            argv += ["--adopt-fd", str(descriptor)]
        return argv

    def start(self, descriptors, env=None, trace_binds=True, extra=()):
        argv = self.command(descriptors, trace_binds) + list(extra)
        handle = open(self.log, "wb")
        try:
            self.process = subprocess.Popen(argv, pass_fds=tuple(descriptors), env=env, stdout=handle,
                                            stderr=subprocess.STDOUT, preexec_fn=os.setsid)
        finally:
            handle.close()

    def stop(self):
        if self.process is None or self.process.poll() is not None:
            return
        group = os.getpgid(self.process.pid)
        os.killpg(group, signal.SIGTERM)
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            try:
                os.killpg(group, 0)
            except ProcessLookupError:
                break
            time.sleep(0.1)
        else:
            os.killpg(group, signal.SIGKILL)
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)

    def text(self, path=None):
        try:
            return open(path or self.log, encoding="utf-8", errors="replace").read()
        except OSError:
            return ""

    def bind_lines(self):
        return [line for line in self.text(self.trace).splitlines()
                if re.match(r"^(?:\[pid \d+\] )?(?:bind|listen)\(", line)]

    def traced(self):
        return "execve(" in self.text(self.trace)


def wait_answered(port, timeout=20.0, context=None, path="/json"):
    """轮询到能拿到 HTTP 响应为止：不赌某一次的时间，也不因为一次慢握手就判死。"""
    factory = http.client.HTTPSConnection if context else http.client.HTTPConnection
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            connection = factory("127.0.0.1", port, timeout=2.0, **({"context": context} if context else {}))
            connection.request("GET", path)
            response = connection.getresponse()
            response.read()
            connection.close()
            if response.status == 200:
                return True
        except OSError:
            pass
        time.sleep(0.1)
    return False


def reset_counter(port, context):
    """从 /metrics 抄一次「服务端交出去的无状态重置条数」；取不到就把原因原样报出来。"""
    request = http.client.HTTPSConnection("127.0.0.1", port, timeout=3.0, context=context)
    try:
        request.request("GET", "/metrics")
        body = request.getresponse().read().decode("utf-8", "replace")
    except OSError as error:
        return "取不到：%s" % error
    finally:
        request.close()
    for line in body.splitlines():
        if line.startswith("asyn_quic_stateless_resets_sent_total "):
            return line.split()[-1]
    return "读数不在 /metrics 里"


def recv_token_match(sock, token):
    """在这把带超时的数据报套接字上收，直到尾部 16 字节等于那枚令牌或这一轮超时。

    走一个线程里的阻塞 recv：asyncio 的 sock_recv 被 wait_for 掐掉时会把已经到达的那个数据报一起
    丢掉（实测同一条报文用阻塞套接字收得到，轮询式收 3 秒内一条也没进来）。
    """
    deadline = time.monotonic() + sock.gettimeout()
    last_error = None
    while time.monotonic() < deadline:
        try:
            candidate = sock.recv(4096)
        except OSError as error:
            last_error = "%s: %s" % (type(error).__name__, error)
            continue
        if bytes(candidate[-16:]) == token:
            return candidate, last_error
    return None, last_error


def load_aioquic():
    """按需引入 aioquic；没有就返回 None，让第 5、6 组如实报「没跑」。"""
    try:
        from aioquic.asyncio.client import connect
        from aioquic.quic.configuration import QuicConfiguration
        from aioquic.quic.connection import QuicConnection
    except ImportError:
        return None
    return connect, QuicConfiguration, QuicConnection


def make_h3_probe_classes(aioquic):
    """一条只够用的 h3 客户端：发 GET、把状态码收回，其余交给 aioquic。"""
    from aioquic.asyncio.protocol import QuicConnectionProtocol
    from aioquic.h3.connection import H3_ALPN, H3Connection
    from aioquic.h3.events import HeadersReceived
    from aioquic.quic.configuration import QuicConfiguration

    class Http3Probe(QuicConnectionProtocol):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            self._http = H3Connection(self._quic)
            self._stream_id = None
            self.status = None
            self.finished = asyncio.Event()

        def send_get(self, authority, path):
            self._stream_id = self._quic.get_next_available_stream_id()
            self._http.send_headers(stream_id=self._stream_id, headers=[
                (b":method", b"GET"), (b":scheme", b"https"),
                (b":authority", authority.encode()), (b":path", path.encode())], end_stream=True)
            self.transmit()

        def quic_event_received(self, event):
            for http_event in self._http.handle_event(event):
                if isinstance(http_event, HeadersReceived) and http_event.stream_id == self._stream_id:
                    for name, value in http_event.headers:
                        if name == b":status":
                            self.status = int(value)
                    if getattr(http_event, "stream_ended", False):
                        self.finished.set()

    def configuration():
        options = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN, server_name="127.0.0.1")
        options.verify_mode = ssl.CERT_NONE
        return options

    return Http3Probe, configuration


async def ask_h3(probe, port):
    probe.status = None
    probe.finished.clear()
    await asyncio.sleep(0.5)
    probe.send_get("127.0.0.1:%d" % port, "/json")
    try:
        await asyncio.wait_for(probe.finished.wait(), timeout=5.0)
    except asyncio.TimeoutError:
        # 状态行到了、而这本探针没等到收尾事件也算：这里要钉的是「这一代在同一个端口上把 h3
        # 请求接住了」，正文完整性由 scripts/h3_acceptance.py 那一份跨实现验收负责
        pass
    return probe.status


async def open_h3_connection(options, port, aioquic, classes):
    """自己造一条连接：只有这样才能让它在「A 退 B 起」这段窗口里一直开着。"""
    connect, _configuration, QuicConnection = aioquic
    Http3Probe, make_configuration = classes
    loop = asyncio.get_running_loop()
    configuration = make_configuration()
    configuration.idle_timeout = CLIENT_IDLE_TIMEOUT_SECONDS
    connection = QuicConnection(configuration=configuration)
    protocol = Http3Probe(quic=connection)
    await loop.create_datagram_endpoint(lambda: protocol, local_addr=("127.0.0.1", 0))
    connection.connect(("127.0.0.1", port), now=time.monotonic())
    protocol.transmit()
    deadline = time.monotonic() + 20.0
    while protocol.status is None and time.monotonic() < deadline:
        try:
            await ask_h3(protocol, port)
        except Exception:  # noqa: BLE001 - 服务端刚起来那几拍没应答，再问一次
            await asyncio.sleep(0.3)
    return protocol


async def run_h3_group(options, port, context):
    """第 5、6 组：数据报那一枚的接手与换代。没有 aioquic 就整组如实报没跑。"""
    aioquic = load_aioquic()
    if aioquic is None:
        check("第 5/6 组（h3 接手与换代）需要 aioquic 才能跑", False, "带 aioquic 的解释器下重跑本脚本")
        return
    connect, _configuration, _QuicConnection = aioquic
    classes = make_h3_probe_classes(aioquic)

    tcp = make_tcp_listener(port)
    udp = make_udp_socket(port)
    key_file = os.path.join(options.workspace, "reset.key")
    key_bytes = os.urandom(32)
    open(key_file, "wb").write(key_bytes)
    extra = ["--https", "--h3", "--cert", options.cert, "--key", options.key,
             "--stateless-reset-key", key_file, "--metrics"]
    first = Server(options, "first", port)
    second = Server(options, "handover", port)
    old_client = None
    try:
        first.start((tcp.fileno(), udp.fileno()), extra=extra)
        old_client = await open_h3_connection(options, port, aioquic, classes)
        check("接手来的数据报套接字上 h3 应答 200（不是只看 alt-svc 的静态通告）", old_client.status == 200,
              "status=%s" % old_client.status)
        peer_cid = old_client._quic._peer_cid.cid
        peer_token = old_client._quic._peer_cid.stateless_reset_token
        check("服务端为这条连接的目的标识签发了无状态重置令牌", len(peer_cid) == CID_LENGTH and len(peer_token) == 16,
              "目的标识 %d 字节，令牌 %d 字节" % (len(peer_cid), len(peer_token)))
        check("这一台没有 bind/listen 过任何描述符（两份都是接手的）", not first.bind_lines() and first.traced(),
              "bind/listen 行 %d 条，strace 跟到 %s" % (len(first.bind_lines()), first.traced()))
        check("日志说的是接手而不是自绑", "TCP 接手" in first.text() and "HTTP/3 接手" in first.text(),
              " | ".join(line[:150] for line in first.text().splitlines() if "接管模式已生效" in line) or "日志里没有那一句")

        first.stop()
        check("交棒的那一台连同它读套接字的那一份都退干净了", not server_pids(options.server, port),
              "还在跑：%s" % server_pids(options.server, port))
        second.start((tcp.fileno(), udp.fileno()), extra=extra)
        new_status = None
        deadline = time.monotonic() + HANDOVER_WAIT_SECONDS
        while time.monotonic() < deadline:
            try:
                async with connect("127.0.0.1", port, configuration=classes[1](), create_protocol=classes[0]) as fresh:
                    new_status = await ask_h3(fresh, port)
                break
            except Exception:  # noqa: BLE001 - 新一代还没起完，等下一拍
                await asyncio.sleep(0.3)
        check("新一代接手后同端口上新开一条 h3 连接应答 200（客户端重连这条路是通的）", new_status == 200,
              "status=%s" % new_status)

        trigger = bytes([0x40]) + peer_cid + bytes([0x01]) + bytes(TRIGGER_DATAGRAM_BYTES - 2 - len(peer_cid))
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sender.bind(("127.0.0.1", 0))
        sender.settimeout(RESET_WAIT_SECONDS)
        sent_at = time.monotonic()
        counter_before = reset_counter(port, context)
        sender.sendto(trigger, ("127.0.0.1", port))
        reply, receive_error = await asyncio.to_thread(recv_token_match, sender, peer_token)
        latency = time.monotonic() - sent_at
        sender.close()
        check("老连接那条目的标识由新一代回了无状态重置（尾部令牌逐字等于交棒方宣告的那枚）", reply is not None,
              "往返 %.3f 秒（上限 %.1f 秒，触发报文 %d 字节，服务端计数 %s→%s，收包错误 %s）"
              % (latency, RESET_WAIT_SECONDS, len(trigger), counter_before, reset_counter(port, context), receive_error))
        if reply is not None:
            check("重置比触发它的那包小（RFC 9000 §10.3.3 的防循环）", len(reply) == TRIGGER_DATAGRAM_BYTES - 1,
                  "%d 字节对 %d 字节" % (len(reply), TRIGGER_DATAGRAM_BYTES))
            recomputed = hmac.new(key_bytes, peer_cid, hashlib.sha256).digest()[:16]
            check("令牌口径与 §10.3.2 一致（HMAC-SHA-256 前 16 字节，两份实现同解）",
                  bytes(reply[-16:]) == recomputed and recomputed == bytes(peer_token),
                  "线上那枚 %s，按密钥现算 %s" % (bytes(reply[-16:]).hex(), recomputed.hex()))
        check("新一代也没有 bind/listen 过任何描述符（换代没重绑）", not second.bind_lines() and second.traced(),
              "bind/listen 行 %d 条：%s" % (len(second.bind_lines()), "; ".join(second.bind_lines()[:2])))
        check("新一代日志说的是接手而不是自绑", "TCP 接手" in second.text() and "HTTP/3 接手" in second.text(),
              " | ".join(line[:150] for line in second.text().splitlines() if "接管模式已生效" in line) or "日志里没有那一句")
    finally:
        first.stop()
        second.stop()
        if old_client is not None:
            try:
                old_client._transport.close()
            except Exception:  # noqa: BLE001 - 收尾不打断读数
                pass
        tcp.close()
        udp.close()


def server_pids(server_path, port):
    """数一遍还在跑的这一台服务进程（按端口认；zombie 的 cmdline 是空的，不会被算进来）。"""
    found = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open("/proc/%s/cmdline" % entry, "rb") as handle:
                line = handle.read().replace(b"\0", b" ").decode("utf-8", "replace")
        except OSError:
            continue
        if server_path in line and ("--port %d" % port) in line and not line.startswith("strace"):
            found.append(entry)
    return found


def run_activation_group(options):
    """第 1..4 组：--adopt-fd、systemd 的两份环境变量、以及不许重绑。"""
    listener = make_tcp_listener(pick_port())
    port = listener.getsockname()[1]
    first = Server(options, "adopt", port)
    first.start((listener.fileno(),))
    answered_before = wait_answered(port)
    listener.close()  # 交棒方放手：端口不能因此出现空窗
    answered_after = wait_answered(port, timeout=8.0)
    check("--adopt-fd 交来的监听器在同一端口应答（交棒方退出前）", answered_before, "port %d" % port)
    check("交棒方关掉自己那一份之后仍应答（端口从不关闭）", answered_after, "port %d" % port)
    check("接管没有重复 bind/listen 任何描述符", not first.bind_lines() and first.traced(),
          "bind/listen 行 %d 条：%s；strace 跟到 %s" % (len(first.bind_lines()), "; ".join(first.bind_lines()[:2]), first.traced()))
    check("日志说明是接管而不是自绑", "接管模式" in first.text() and "TCP 接手" in first.text(),
          first.text().strip().splitlines()[-1][:120] if first.text().strip() else "无日志")
    first.stop()
    listener.close()

    # systemd 的 LISTEN_FDS 从 3 起算：用 sh -c + exec 让服务进程的 pid 就是 LISTEN_PID 里那个
    listener2 = make_tcp_listener(pick_port())
    port2 = listener2.getsockname()[1]
    second = Server(options, "activation", port2)
    shell = "LISTEN_PID=$$ LISTEN_FDS=1 exec %s --host 127.0.0.1 --port %d" % (options.server, port2)
    handle = open(second.log, "wb")
    try:
        second.process = subprocess.Popen(["sh", "-c", shell], pass_fds=(listener2.fileno(),), stdout=handle,
                                          stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    finally:
        handle.close()
    answered2 = wait_answered(port2)
    listener2.close()
    answered2_after = wait_answered(port2, timeout=8.0)
    check("LISTEN_FDS 交来的描述符被接手并在同一端口应答", answered2 and answered2_after, "前后 %s/%s" % (answered2, answered2_after))
    check("走的是 systemd 那一路（日志有接管形状）", "接管模式" in second.text(),
          second.text().strip().splitlines()[-1][:120] if second.text().strip() else "无日志")
    second.stop()
    listener2.close()

    port3 = pick_port()
    third = Server(options, "mismatch", port3)
    env = dict(os.environ)
    env["LISTEN_PID"] = "1"      # init 的 pid：一定不是本进程
    env["LISTEN_FDS"] = "3"      # 声称交来 3 枚（fd 3..5，本进程里其实没有）
    third.start((), env=env, trace_binds=False)
    answered3 = wait_answered(port3)
    mismatch_line = [line[:160] for line in third.text().splitlines() if "忽略" in line]
    check("LISTEN_PID 不匹配时忽略并打一行说明", "忽略 systemd 交来的 3 枚描述符" in third.text(),
          mismatch_line[0] if mismatch_line else "日志里没有那一句")
    check("忽略之后按老路自己 bind 并服务", answered3, "port %d" % port3)
    third.stop()


def main():
    parser = argparse.ArgumentParser(description="监听套接字交接与 socket activation 的进程外实测")
    parser.add_argument("--server", required=True, help="ReferenceServer 可执行体路径")
    parser.add_argument("--cert", required=True, help="h3 那两组要用的证书")
    parser.add_argument("--key", required=True, help="上面那份证书的私钥")
    options = parser.parse_args()
    options.workspace = tempfile.mkdtemp(prefix="fd-takeover-")
    options.server = os.path.abspath(options.server)
    options.cert = os.path.abspath(options.cert)
    options.key = os.path.abspath(options.key)

    for path, label in ((options.server, "服务可执行体"), (options.cert, "证书"), (options.key, "私钥")):
        if not os.path.exists(path):
            print("缺少%s：%s" % (label, path), file=sys.stderr)
            return 2
    if not os.environ.get("ASYN_KEEP_TAKEOVER_LOGS"):
        pass  # 现场目录留在临时区，读数都打在 stdout 上；要留档就把这个环境变量置上

    context = ssl._create_unverified_context()
    if os.name != "posix":
        print("本平台不适用：描述符不随 exec 继承，交接那两条入口在 Windows 上没有对应形状")
        return 0
    run_activation_group(options)
    asyncio.run(run_h3_group(options, pick_port(), context))

    print()
    print("判据失败清单：%s" % (FAILURES if FAILURES else "无"))
    print("现场目录：%s" % options.workspace)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
