#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""弱错误断言与固定睡眠清单：只报告，不阻断。

判据形状（P3 的账本，不是门禁）：
- 弱断言：EXPECT_THROW / ASSERT_THROW 起手 6 行内没有任何错误读数（what()、lastError()、
  error()、errorMessage、reason 一类）。这类用例只证明「抛了」，改掉文案、漏掉资源回收都照样绿。
- 固定睡眠：sleep_for 起手 10 行内出现 EXPECT_/ASSERT_，这类用例靠睡够推进，慢机器上爱假红。

用法：python3 scripts/report-weak-error-assertions.py [--json]
退出码恒为 0；参数读不到或 git 不可用时退 2（清单没跑成，不能读成零条）。
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

THROW_PATTERN = re.compile(r"\b(?:EXPECT|ASSERT)_(?:ANY_)?THROW\b")
ASSERTION_PATTERN = re.compile(r"\b(?:EXPECT|ASSERT)_[A-Z_]+\b")
SLEEP_PATTERN = re.compile(r"\bsleep_for\s*\(")
# 读过错误正文或状态才算有判据；不是逐字清单，缺了就在人工复核时补
STRONG_MARKERS = ("what()", "lastError(", ".error(", "errorMessage", "failureText", ".reason", "reason.", "statusCode")
# 断言视窗：起手行之后还要看几行（多于一行是因为断言常写成两行，读数写在下一句）
ASSERTION_WINDOW = 6
SLEEP_WINDOW = 10
# 第一批（安全相关）的模块分组：路径前缀 → 批次名
MODULE_GROUPS = (
    ("tests/Core/Tls", "TLS"),
    ("tests/Net/Http/TestHttpsServer", "TLS"),
    ("tests/Core/Tls/TestTlsPolicy", "TLS"),
    ("tests/Database/Redis", "Redis"),
    ("tests/Net/Proxy", "Proxy"),
    ("tests/Net/Http/TestHttpProxy", "Proxy"),
    ("tests/Net/Acme", "Auth"),
    ("tests/Net/Http/TestJwt", "Auth"),
    ("tests/Net/Http/TestJwks", "Auth"),
    ("tests/Net/Http/TestOAuth", "Auth"),
    ("tests/Net/Http/TestBasicAuth", "Auth"),
    ("tests/Net/Http/TestSecurityHeaders", "Auth"),
    ("tests/Net/Http/TestHttpStatic", "Static"),
    ("tests/Net/Http/TestFileSender", "Static"),
    ("tests/Net/Http/TestHttpDirectoryListing", "Static"),
    ("tests/Net/Http/TestStaticFileMapping", "Static"),
)


def tracked_test_files() -> list[str]:
    result = subprocess.run(["git", "-C", str(REPOSITORY_ROOT), "ls-files", "tests"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("git ls-files 失败，清单没有跑成")
    return [item for item in result.stdout.splitlines() if item.endswith(".cpp")]


def module_of(path: str) -> str:
    for prefix, name in MODULE_GROUPS:
        if path.startswith(prefix):
            return name
    return "Other"


def scan_file(path: str) -> tuple[list[dict], list[dict]]:
    weak: list[dict] = []
    sleeps: list[dict] = []
    lines = (REPOSITORY_ROOT / path).read_text(encoding="utf-8", errors="replace").splitlines()
    for index, line in enumerate(lines):
        if THROW_PATTERN.search(line):
            window = "\n".join(lines[index:index + ASSERTION_WINDOW])
            if not any(marker in window for marker in STRONG_MARKERS):
                weak.append({"path": path, "line": index + 1, "text": line.strip()})
        if SLEEP_PATTERN.search(line):
            # 断言自身那一行不算：睡在断言前的形状是「睡完再看结果」
            followers = lines[index + 1:index + SLEEP_WINDOW]
            if any(ASSERTION_PATTERN.search(item) for item in followers):
                sleeps.append({"path": path, "line": index + 1, "text": line.strip()})
    return weak, sleeps


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="输出机读 JSON（逐条带路径与行号）")
    arguments = parser.parse_args()

    try:
        files = tracked_test_files()
    except RuntimeError as error:
        sys.stderr.write(str(error) + "\n")
        return 2

    weak_entries: list[dict] = []
    sleep_entries: list[dict] = []
    for path in files:
        weak, sleeps = scan_file(path)
        weak_entries.extend(weak)
        sleep_entries.extend(sleeps)

    for entry in weak_entries:
        entry["module"] = module_of(entry["path"])
    for entry in sleep_entries:
        entry["module"] = module_of(entry["path"])

    modules: dict[str, dict[str, int]] = {}
    for entry in weak_entries:
        bucket = modules.setdefault(entry["module"], {"weak": 0, "sleep": 0})
        bucket["weak"] += 1
    for entry in sleep_entries:
        bucket = modules.setdefault(entry["module"], {"weak": 0, "sleep": 0})
        bucket["sleep"] += 1

    if arguments.json:
        print(json.dumps({"weak": weak_entries, "sleep": sleep_entries, "modules": modules}, ensure_ascii=False, indent=2))
        return 0

    for entry in weak_entries:
        print("WEAK_THROW %s:%d %s" % (entry["path"], entry["line"], entry["text"]))
    for entry in sleep_entries:
        print("SLEEP_BEFORE_ASSERT %s:%d %s" % (entry["path"], entry["line"], entry["text"]))
    print("WEAK_TOTAL=%d SLEEP_TOTAL=%d" % (len(weak_entries), len(sleep_entries)))
    for name in sorted(modules, key=lambda item: (item == "Other", item)):
        bucket = modules[name]
        print("MODULE %s weak=%d sleep=%d" % (name, bucket["weak"], bucket["sleep"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
