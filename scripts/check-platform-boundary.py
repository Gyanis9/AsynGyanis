#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Platform 边界门：src/Platform 之外不许出现系统头、OS API 与平台条件分支。

判据：
  * 系统头：windows.h / winsock2.h / unistd.h / pthread.h / sys/* / linux/* / netinet/* 等；
  * 平台条件：含 ASYN_PLATFORM_*、_WIN32、__linux__、__unix__、__APPLE__ 的 #if 族；
  * OS API：全局作用域的 socket/进程/文件系统调用，以及 Windows API 与 std::signal；
  * 基线按 (路径, 类别, 符号) 聚合计数，只准减不准增；例外必须带理由与复核日期。

用法：python scripts/check-platform-boundary.py [--root PATH] [--baseline PATH] [--write] [--min-files N]
退出码：0 通过；1 违例或基线陈旧；2 前置缺件。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from datetime import date
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".inl"}
OS_HEADERS = {
    "windows.h",
    "winsock2.h",
    "ws2tcpip.h",
    "mswsock.h",
    "unistd.h",
    "pthread.h",
    "fcntl.h",
    "poll.h",
    "dirent.h",
    "dlfcn.h",
    "signal.h",
    "termios.h",
    "io_uring.h",
    "sys/epoll.h",
    "sys/eventfd.h",
    "sys/timerfd.h",
    "sys/inotify.h",
    "sys/mman.h",
    "sys/socket.h",
    "sys/stat.h",
    "sys/un.h",
    "sys/wait.h",
    "sys/resource.h",
    "sys/sendfile.h",
    "sys/uio.h",
    "sys/utsname.h",
    "netinet/in.h",
    "netinet/tcp.h",
    "arpa/inet.h",
}
OS_HEADER_PREFIXES = ("sys/", "linux/", "mach/", "netinet/", "arpa/", "bits/")
PLATFORM_TOKENS = ("ASYN_PLATFORM_WIN32", "ASYN_PLATFORM_LINUX", "_WIN32", "__linux__", "__unix__", "__APPLE__")
COND_RE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b")
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*<([^>]+)>")
GLOBAL_CALL_RE = re.compile(r"(?<![A-Za-z0-9_>:])::([A-Za-z_][A-Za-z0-9_]*)\s*\(")
UNQUALIFIED_CALL_RE = re.compile(r"(?<![A-Za-z0-9_>.:])(getpeername|getsockname)\s*\(")
STD_SIGNAL_RE = re.compile(r"\bstd::signal\s*\(")
WIN_API_RE = re.compile(
    r"\b(CreateIoCompletionPort|GetQueuedCompletionStatus|PostQueuedCompletionStatus|"
    r"CreateFile[AW]?|ReadFile|WriteFile|GetLastError|WSARecv|WSASend|AcceptEx|ConnectEx|"
    r"WSAIoctl|WSAGetLastError|WSAStartup|WSACleanup|CloseHandle|SetConsoleCtrlHandler|"
    r"RegisterServiceCtrlHandlerEx|StartServiceCtrlDispatcher|SetServiceStatus)\s*\("
)
OS_CALL_NAMES = {
    "socket",
    "bind",
    "listen",
    "accept",
    "accept4",
    "connect",
    "send",
    "recv",
    "sendto",
    "recvfrom",
    "sendmsg",
    "recvmsg",
    "shutdown",
    "getsockopt",
    "setsockopt",
    "getsockname",
    "getpeername",
    "getaddrinfo",
    "freeaddrinfo",
    "inet_pton",
    "inet_ntop",
    "sendfile",
    "splice",
    "open",
    "openat",
    "close",
    "read",
    "write",
    "pread",
    "pwrite",
    "fsync",
    "fdatasync",
    "stat",
    "fstat",
    "lstat",
    "unlink",
    "rename",
    "mkdir",
    "rmdir",
    "opendir",
    "readdir",
    "closedir",
    "mmap",
    "munmap",
    "mprotect",
    "msync",
    "fork",
    "vfork",
    "execv",
    "execve",
    "execvp",
    "waitpid",
    "wait",
    "pipe",
    "pipe2",
    "socketpair",
    "kill",
    "raise",
    "sigaction",
    "sigprocmask",
    "sigtimedwait",
    "signal",
    "ioctl",
    "fcntl",
    "dup",
    "dup2",
    "select",
    "poll",
    "epoll_create1",
    "epoll_ctl",
    "epoll_wait",
    "io_uring_setup",
    "io_uring_enter",
    "inotify_init1",
    "inotify_add_watch",
    "inotify_rm_watch",
    "dlopen",
    "dlsym",
    "dlclose",
    "getenv",
    "localtime_r",
    "gmtime_r",
    "clock_gettime",
    "gettimeofday",
    "nanosleep",
}
BUILTIN_EXCEPTIONS = {
    ("src/AsynGyanisExport.h", "platform-conditional", "_WIN32"): (
        "导出宏头按平台选 __declspec 与可见性属性，平台分支是它的职责；其余分支由 CMake 注入宏控制。"
    ),
}
DEFAULT_REVIEW_AFTER = "2026-12-31"


def strip_comments(text: str) -> str:
    output = list(text)
    index = 0
    state = "code"
    length = len(text)
    while index < length:
        character = text[index]
        if state == "code":
            if text.startswith("//", index):
                output[index] = " "
                output[index + 1] = " "
                state = "line"
                index += 2
                continue
            if text.startswith("/*", index):
                output[index] = " "
                output[index + 1] = " "
                state = "block"
                index += 2
                continue
            if character == '"':
                state = "string"
            elif character == "'":
                state = "char"
            index += 1
        elif state == "line":
            if character == "\n":
                state = "code"
            else:
                output[index] = " "
            index += 1
        elif state == "block":
            if text.startswith("*/", index):
                output[index] = " "
                output[index + 1] = " "
                state = "code"
                index += 2
                continue
            if character != "\n":
                output[index] = " "
            index += 1
        else:
            if character == "\\" and index + 1 < length:
                index += 2
                continue
            if (state == "string" and character == '"') or (state == "char" and character == "'"):
                state = "code"
            index += 1
    return "".join(output)


def classify(path: Path) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    for line in strip_comments(path.read_text(encoding="utf-8", errors="replace")).splitlines():
        include = INCLUDE_RE.match(line)
        if include:
            header = include.group(1).strip()
            if header in OS_HEADERS or header.startswith(OS_HEADER_PREFIXES):
                findings.append(("os-header", header))
        if COND_RE.match(line):
            for token in PLATFORM_TOKENS:
                if re.search(r"\b" + re.escape(token) + r"\b", line):
                    findings.append(("platform-conditional", token))
                    break
        for call in GLOBAL_CALL_RE.finditer(line):
            if call.group(1) in OS_CALL_NAMES:
                findings.append(("os-api", "::" + call.group(1)))
        for call in UNQUALIFIED_CALL_RE.finditer(line):
            findings.append(("os-api", call.group(1)))
        if STD_SIGNAL_RE.search(line):
            findings.append(("os-api", "std::signal"))
        for win in WIN_API_RE.finditer(line):
            findings.append(("os-api", win.group(1)))
    return findings


def scan(root: Path) -> tuple[Counter, int]:
    counter: Counter = Counter()
    files = 0
    for path in sorted((root / "src").rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(root).as_posix()
        if relative.startswith("src/Platform/"):
            continue
        files += 1
        for category, symbol in classify(path):
            counter[(relative, category, symbol)] += 1
    return counter, files


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--baseline", default=None)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--min-files", type=int, default=300)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    baseline_path = (
        Path(args.baseline)
        if args.baseline
        else Path(__file__).resolve().parent / "platform-boundary-baseline.json"
    )

    if not (root / "src").is_dir():
        print(f"PLATFORM_BOUNDARY_GATE=ERROR 找不到 {root / 'src'}", file=sys.stderr)
        return 2

    current, files = scan(root)
    if files < args.min_files:
        print(f"PLATFORM_BOUNDARY_GATE=ERROR 只扫到 {files} 个源文件（下限 {args.min_files}）", file=sys.stderr)
        return 2

    exceptions = {key: reason for key, reason in BUILTIN_EXCEPTIONS.items() if key in current}
    actionable = Counter({key: count for key, count in current.items() if key not in exceptions})

    if args.write:
        payload = {
            "version": 1,
            "exceptions": [
                {
                    "path": key[0],
                    "category": key[1],
                    "symbol": key[2],
                    "reason": reason,
                    "reviewAfter": DEFAULT_REVIEW_AFTER,
                }
                for key, reason in sorted(exceptions.items())
            ],
            "violations": [
                {"path": key[0], "category": key[1], "symbol": key[2], "count": count}
                for key, count in sorted(actionable.items())
            ],
        }
        baseline_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(
            f"PLATFORM_BOUNDARY_GATE=WRITE files={files} actionable={len(actionable)} exceptions={len(exceptions)}"
        )
        return 0

    if not baseline_path.is_file():
        print(f"PLATFORM_BOUNDARY_GATE=ERROR 读不到基线 {baseline_path}；先跑 --write 并在提交里说明", file=sys.stderr)
        return 2

    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    recorded = {
        (entry["path"], entry["category"], entry["symbol"]): int(entry.get("count", 1))
        for entry in baseline.get("violations", [])
    }
    allowed = {
        (entry["path"], entry["category"], entry["symbol"]): entry for entry in baseline.get("exceptions", [])
    }

    problems: list[str] = []
    for key, entry in allowed.items():
        if key not in current:
            problems.append(f"例外已失效：{key[0]} {key[1]} {key[2]}")
        review_after = entry.get("reviewAfter", "")
        if review_after and review_after < date.today().isoformat():
            problems.append(f"例外已过期：{key[0]} {key[1]} {key[2]}（复核日期 {review_after}）")

    for key, count in actionable.items():
        if key not in recorded:
            problems.append(f"新增违例：{key[0]} {key[1]} {key[2]} x{count}")
        elif count > recorded[key]:
            problems.append(f"违例增加：{key[0]} {key[1]} {key[2]} {recorded[key]} -> {count}")
        elif count < recorded[key]:
            problems.append(f"基线陈旧：{key[0]} {key[1]} {key[2]} {recorded[key]} -> {count}，跑 --write 收紧")
    for key in recorded:
        if key not in actionable:
            problems.append(f"基线陈旧：{key[0]} {key[1]} {key[2]} 已消失，跑 --write 收紧")

    if problems:
        print(f"PLATFORM_BOUNDARY_GATE=FAIL files={files} problems={len(problems)}")
        for problem in problems:
            print(f"  {problem}")
        return 1

    print(f"PLATFORM_BOUNDARY_GATE=OK files={files} actionable=0 exceptions={len(allowed)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
