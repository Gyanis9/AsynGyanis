#!/usr/bin/env python3
"""WebSocket 一致性验收：用 websockets 这套独立实现当对端，把 RFC 6455 的几条主干路径走一遍。

理由与 h3 探针一样：两边都照规范办事，谁理解错了都会在这里露出来，自写客户端只会重复本框架自己的
误会。本框架自带的用例与示例已经把「帧进得来、出得去」与一批畸形帧判废钉住了，这里补的是**跨实现**
那一半：真实客户端的分片重组、UTF-8 与 astral 字符、控制帧应答、关闭握手的完整往返、
permessage-deflate（RFC 7692）协商之后大正文仍然等价。

用法：
    python3 ws_interop_probe.py <host> <port> [--path /ws] [--wss] [--no-deflate]
退出码 0 表示全部核对通过；任一不通过时把原因打到 stderr 并以 1 退出；启动参数不合法退 2。
在 Git Bash/MSYS 下传 --path 要加 MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'（见 main 里的判据）。

先起服务（明文示例）：
    build/debug/samples/echo_server.exe --port 18080
TLS 版：
    build/debug/samples/echo_server.exe --port 18443 --https --cert tests/Core/fixtures/test_cert.pem
        --key tests/Core/fixtures/test_key.pem

依赖：pip install websockets
"""

import argparse
import asyncio
import ssl
import sys

import websockets

# 结论行既给人读也被脚本 grep：Windows 控制台默认按 OEM 码页（zh-CN 是 936）出字，中文会打成乱码，
# 因此一律按 UTF-8 写出去（与 benchmarks/check-baseline.py 需要 PYTHONIOENCODING 是同一件事，
# 这里由探针自己保证，不再要求调用方设环境变量）
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

# 每一步的等待上限：这是验收探针不是压测，超时一律按「服务端没按规范办事」判失败
STEP_TIMEOUT_SECONDS = 10

# 大正文的字节数：既够把 permessage-deflate 真正压起来，也远在本框架 8 MiB 的消息上限之内
LARGE_PAYLOAD_BYTES = 256 * 1024

# 分片时的片数：两条 continuation 就足够让「按帧回显、没重组」的实现露出来
FRAGMENT_COUNT = 3

# 本框架不协商业务子协议，探针也不声明任何应用层协议
SUBPROTOCOLS = []


def fail(reason: str) -> None:
    """把一条不通过打到 stderr，并让进程最终以非零码退出。"""
    print(f"FAIL: {reason}", file=sys.stderr, flush=True)
    fail.count += 1


fail.count = 0


def ok(step: str) -> None:
    print(f"OK   {step}", flush=True)


async def timed(awaitable, step: str):
    """给一步套上时限：超时也算失败，绝不让探针自己挂在没有响应的连接上。"""
    try:
        return await asyncio.wait_for(awaitable, timeout=STEP_TIMEOUT_SECONDS)
    except asyncio.TimeoutError:
        fail(f"{step}：{STEP_TIMEOUT_SECONDS} 秒内没有等到响应")
        return None
    except websockets.exceptions.WebSocketException as error:
        fail(f"{step}：独立实现侧报错 {type(error).__name__}: {error}")
        return None


async def echo_back(connection, payload: str, step: str):
    """发一条并取回 echoed 文本；不一致或没回来就把原因记下来。"""
    await timed(connection.send(payload), f"{step}：发送")
    received = await timed(connection.recv(), f"{step}：等回显")
    if received is None:
        return None
    if isinstance(received, (bytes, bytearray)):
        received = received.decode("utf-8", errors="replace")
        fail(f"{step}：路由只回文本帧，却收到二进制帧")
    if received != payload:
        fail(f"{step}：回显与送出的不等长（送出 {len(payload)}，收到 {len(received) if received else 0}）")
        return None
    return received


async def run_one(url: str, label: str, check_deflate: bool) -> None:
    """一条连接上把该走的都走一遍；url 用 ws:// 或 wss:// 由调用方决定。"""
    ssl_context = None
    if url.startswith("wss://"):
        # 夹具证书是自签的：本探针要验的是协议而不是证书链（与 h3 探针同一口径）
        ssl_context = ssl.create_default_context()
        ssl_context.check_hostname = False
        ssl_context.verify_mode = ssl.CERT_NONE

    # 客户端的 max_size 要放得下回显的大正文，否则「服务端没回全」与「客户端自己拒收」分不开
    try:
        connection = await websockets.connect(url, subprotocols=SUBPROTOCOLS, ssl=ssl_context,
                                              max_size=4 * LARGE_PAYLOAD_BYTES, open_timeout=STEP_TIMEOUT_SECONDS)
    except (websockets.exceptions.WebSocketException, OSError) as error:
        # 握手没成要报成一条失败，而不是甩一段 traceback：这条探针是给CI读的
        fail(f"{label}：握手没成（{type(error).__name__}: {error}）")
        return

    async with connection:
        ok(f"{label}：握手完成并进入打开态")

        # 1. 文本回显：ASCII、中文、以及需要代理对的 astral 字符一起放一条里
        sample = "hello 你好 🜁 A🜂 — 6455 §5.6 的 UTF-8 边界"
        if await echo_back(connection, sample, f"{label}/文本回显") is not None:
            ok(f"{label}：文本回显一致（{len(sample)} 码点，含代理对）")

        # 2. 大正文回显：带 permessage-deflate 时这一步同时验压缩往返
        large = ("compressible payload " * (LARGE_PAYLOAD_BYTES // 23 + 1))[:LARGE_PAYLOAD_BYTES]
        if await echo_back(connection, large, f"{label}/大正文回显") is not None:
            ok(f"{label}：{LARGE_PAYLOAD_BYTES} 字节正文原样回来")

        # 3. 分片重组：走 websockets 自己的成帧代码（掩码、帧头与 permessage-deflate 都由它生成），
        #    分片是**连续切段**，拼回去必须正好等于原消息——这条自证放在发送前，免得把探针自己的错
        #    算到服务端头上。协商了 deflate 之后每一片都是独立压缩块（client_no_context_takeover），
        #    片尾还要带 Z_SYNC_FLUSH，这是跨实现最容易各写各的一处。
        #    按帧回显、没把 continuation 拼回去的实现，在这一步收到的就是三分之一的长度。
        send_context = getattr(connection, "send_context", None)
        if send_context is None:
            print(f"SKIP {label}：本版 websockets 没有 send_context，分片这一项未由对端驱动", flush=True)
        else:
            piece_length = (len(large) + FRAGMENT_COUNT - 1) // FRAGMENT_COUNT
            fragments = [large[i:i + piece_length] for i in range(0, len(large), piece_length)]
            if "".join(fragments) != large:
                fail(f"{label}/分片自证：切段拼不回原消息，探针自己的分片算错了")
            else:
                try:
                    async def send_fragmented() -> None:
                        # 这一层的成帧接口收的是字节（扩展在它自己那层压缩），文本要先编码
                        async with connection.send_context():
                            connection.protocol.send_text(fragments[0].encode(), fin=False)
                            for index, fragment in enumerate(fragments[1:], start=1):
                                connection.protocol.send_continuation(fragment.encode(), fin=index == len(fragments) - 1)

                    await asyncio.wait_for(send_fragmented(), timeout=STEP_TIMEOUT_SECONDS)
                except (asyncio.TimeoutError, websockets.exceptions.WebSocketException) as error:
                    fail(f"{label}/分片发送：{type(error).__name__}: {error}")
                else:
                    received = await timed(connection.recv(), f"{label}：等分片重组")
                    if received is not None:
                        if received != large:
                            fail(f"{label}/分片重组：收到的不是拼回去的那条"
                                 f"（{len(received) if received else 0} vs {len(large)}，"
                                 f"像是按帧回显而不是按消息回显）")
                        else:
                            ok(f"{label}：{len(fragments)} 个压缩分片重组为一条完整消息")

        # 4. 控制帧：ping 必须被 pong 回来（RFC 6455 §5.5.2/§5.5.3）
        try:
            pong_waiter = await asyncio.wait_for(connection.ping(), timeout=STEP_TIMEOUT_SECONDS)
            await asyncio.wait_for(pong_waiter, timeout=STEP_TIMEOUT_SECONDS)
            ok(f"{label}：ping 收到 pong")
        except (asyncio.TimeoutError, websockets.exceptions.WebSocketException) as error:
            fail(f"{label}/ping-pong：{type(error).__name__}")

        # 5. permessage-deflate 协商：响应里只允许出现我们确实支持的扩展
        advertised = connection.response.headers.get("Sec-WebSocket-Extensions", "")
        if check_deflate and "permessage-deflate" not in advertised and advertised:
            fail(f"{label}/扩展：服务端回了不认识的扩展「{advertised}」")
        elif advertised and "permessage-deflate" not in advertised:
            fail(f"{label}/扩展：客户端提议了 permessage-deflate 却收到别的扩展「{advertised}」")
        else:
            ok(f"{label}：扩展协商 = {advertised if advertised else '（未协商，两端各自原文传输）'}")

        # 6. 关闭握手：客户端发 1000，必须收到对端的 1000 作为应答（只发不答的实现在这里红）
        close_reason = f"{label}/关闭握手"
        try:
            await asyncio.wait_for(connection.close(code=1000, reason="验收结束"), timeout=STEP_TIMEOUT_SECONDS)
            await asyncio.wait_for(connection.wait_closed(), timeout=STEP_TIMEOUT_SECONDS)
        except (asyncio.TimeoutError, websockets.exceptions.WebSocketException) as error:
            fail(f"{close_reason}：{type(error).__name__}: {error}")
            return
        if connection.close_code != 1000:
            fail(f"{close_reason}：服务端回的关闭码是 {connection.close_code}，规范要求应答同一状态码")
        else:
            ok(f"{close_reason}：双向 1000 收口")


async def run(args) -> None:
    scheme = "wss" if args.wss else "ws"
    url = f"{scheme}://{args.host}:{args.port}{args.path}"
    await run_one(url, "wss/TLS" if args.wss else "ws/明文", not args.no_deflate)


def main() -> int:
    parser = argparse.ArgumentParser(description="WebSocket 跨实现一致性验收")
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("--path", default="/ws", help="echo_server 的 WebSocket 路由")
    parser.add_argument("--wss", action="store_true", help="走 TLS（服务端需带 --https --cert --key）")
    parser.add_argument("--no-deflate", action="store_true", help="核对时不要求 permessage-deflate 协商结果")
    args = parser.parse_args()

    # 路径必须以 / 开头：从 Git Bash 传「/json」这类参数会被 MSYS 换成本机绝对路径，拼进 URI 之后
    # 连端口都跟着变形（实测报 ValueError: Port could not be cast ... '18080C:'）。那种失败既不是
    # 服务端的错也不该以 traceback 收场，所以当场拒掉并说清楚解法。
    if not args.path.startswith("/"):
        print(f"启动参数非法：--path 要以 / 开头，收到「{args.path}」。从 MSYS/Git Bash 传以 / 开头的"
              "参数时，请在命令前加 MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'", file=sys.stderr)
        return 2

    asyncio.run(run(args))

    if fail.count:
        print(f"\n共 {fail.count} 项不通过", file=sys.stderr)
        return 1
    print("\n全部核对通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
