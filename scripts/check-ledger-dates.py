#!/usr/bin/env python3
"""台账日期的机检：文档里写成 <提交号>（YYYY-MM-DD）的日期须与该提交的作者日期同解。

追溯门逐行读这几份 markdown 里的「路径:行号」，判的只是那一行在不在、是不是空行；日期与该提交
的作者日期是否同解它判不了，台账自相矛盾只能由本门抓。
判据只吃「提交号紧跟全角括号与日期」的紧形状，不吃长句里的日期（常指别的事，硬判会误报）。
2026-10-11 首跑实测：紧形状命中 7 处，其中 2 处与 git 不同解。
退出码：0 同解；1 有不一致或提交号查不到；2 这道门没跑成（不是 git、扫描清单为空、
文件读不到）——空清单被读成通过是最坏的那种假绿。
"""

import io
import os
import re
import subprocess
import sys

REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 台账就在这几份文件里；新增载体时改这一行，别在别处再写一份清单
LEDGER_FILES = (
    "README.md",
    "CHANGELOG.md",
    ".github/SECURITY.md",
    "CONTRIBUTING.md",
    "docs/入门与构建.md",
    "docs/架构设计.md",
    "docs/网络编程.md",
    "docs/数据库访问.md",
    "docs/部署与运维.md",
    "docs/配置与可观测性.md",
    "docs/测试与验证.md",
    "docs/性能测试.md",
)

TIGHT_CITATION = re.compile(r"`([0-9a-f]{7,40})`（(20\d\d-\d\d-\d\d)")


def authorDate(commit: str) -> str | None:
    """取该提交自己的作者日期（short 档，即提交者当时本地那一天）。查不到返回 None。"""
    result = subprocess.run(
        ["git", "log", "-1", "--format=%ad", "--date=short", f"{commit}^{{commit}}"],
        cwd=REPOSITORY_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def main() -> int:
    # 中文读数在 cp1252/cp936 的控制台上会抛 UnicodeEncodeError，先把两个流钉成 UTF-8
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    if subprocess.run(["git", "rev-parse", "--git-dir"], cwd=REPOSITORY_ROOT, capture_output=True).returncode != 0:
        print("无法执行：这里不是 git 仓库，取不到提交日期")
        return 2

    citations: list[tuple[str, str, str, int]] = []
    scannedFiles = 0
    for relativePath in LEDGER_FILES:
        path = os.path.join(REPOSITORY_ROOT, relativePath)
        if not os.path.isfile(path):
            print(f"无法执行：台账文件读不到（{relativePath}）")
            return 2
        scannedFiles += 1
        text = io.open(path, encoding="utf-8", errors="replace").read()
        for line in text.splitlines():
            for commit, date in TIGHT_CITATION.findall(line):
                citations.append((relativePath, commit, date, len(citations) + 1))

    if not citations:
        print(f"无法执行：{scannedFiles} 份台账文件里一条紧形状的「提交号（日期）」都没扫到——判据没吃到东西")
        return 2

    print(f"== 台账日期机检：紧形状命中 {len(citations)} 处 ==")
    violations: list[str] = []
    cache: dict[str, str | None] = {}
    for relativePath, commit, date, _ in citations:
        if commit not in cache:
            cache[commit] = authorDate(commit)
        real = cache[commit]
        if real is None:
            print(f"     [FAIL] {relativePath}：`{commit}` 查不到这个提交")
            violations.append(f"{relativePath}：`{commit}` 不是本仓库的提交")
            continue
        if real != date:
            print(f"     [FAIL] {relativePath}：`{commit}` 文中写 {date}，git 的作者日期是 {real}")
            violations.append(f"{relativePath}：`{commit}` 的日期 {date} 与 git 的 {real} 不同解")
        else:
            print(f"     [OK  ] {relativePath}：`{commit}` {date}")

    print(f"== 汇总：不一致 {len(violations)} 条 / 命中 {len(citations)} 处 ==")
    if violations:
        for violation in violations:
            print(f"  FAIL: {violation}")
        return 1
    print("  OK: 台账里每一处紧形状的提交日期都与 git 同解")
    return 0


if __name__ == "__main__":
    sys.exit(main())
