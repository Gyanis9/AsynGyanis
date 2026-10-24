#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""台账正文可追溯性检查：README、CHANGELOG 最新发布段、SECURITY 与 docs 八份专题文档里
手写的每一条「路径:行号」都要当场读得到。

路径不存在、行号越过文件末尾、引到空行 → FAIL；引到注释行 → WARN；区间尾巴越界或倒挂 → FAIL；
只写「:123」或「、123」而前面没有完整路径的一律略过（续写没有可继承对象，判红只会逼人改文案）。
扫描清单里一份文件都读不到、或提取数塌到地板以下 → 退 2：那是什么都没查，不许读成通过。
图住在正文的 mermaid 块里，引用一律落在图下方的「证据：路径:行号」行，本门按原始行扫描、
不剥代码栅栏，因此这些行同样被计数与校验。
用法：python3 scripts/check-arch-traceability.py [--root PATH] [--min-refs N]
退出码：0 通过；1 有违例；2 判据没跑成。
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

# Windows runner 的控制台默认按 cp1252 编码：违例文案是中文，一 print 就抛 UnicodeEncodeError，
# 于是「红是红了，但没说是哪条引用」。与 check-ledger-dates.py 同形状，先钉两个流
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 注释行的形状：C/C++ 与 CMake/YAML/JSON/Python 的几种起始符。判据只用于「引到注释要不要警告」，
# 不参与任何 FAIL 判定，所以宁可写宽一点——把代码行误判成注释只会漏一条 WARN。
COMMENT_PREFIXES = ("//", "*", "/*", "#", ";;", "<!--")
# 正文里手写的「路径:行号」有四种形状：完整路径、只带后缀、单冒号续写、顿号续写，后三种都从
# 前一个完整路径继承所属文件，所以一段正文一次扫完、按出现顺序接力：续写才找得着主人。
SOURCE_EXTS = r"cpp|h|hpp|py|sh|json|ya?ml|md|txt|cmake"
PROSE_REF_RE = re.compile(
    rf"(?P<path>[A-Za-z0-9_./+-]+\.(?:{SOURCE_EXTS})):(?P<full>\d+)"
    rf"|(?P<ext>\.{SOURCE_EXTS}):(?P<suffixed>\d+)"
    rf"|(?<![\w.]):(?P<colon>\d+)"
    rf"|、(?P<comma>\d+)(?![-\d.])"
)
# 正文里的行号区间尾巴：path:40-56 与 path:40..56 两种写法都在用
PROSE_RANGE_RE = re.compile(r"^(?:-(?P<dash>\d+)|\.\.(?P<dots>\d+))")
# 台账正文里也手写「路径:行号」，同一条底线要同判：SECURITY 全篇（安全面承诺的行号要活到兑现）
# 与 CHANGELOG 的最新发布段（发布说明里的引用是对该版本交付物的主张）；旧发布段不回判，那是对
# 已经交付那一版的陈述，后来的提交把行挪走不算它说谎
LEDGER_PROSE_FILES = (
    ".github/SECURITY.md", "CHANGELOG.md", "README.md",
    "docs/入门与构建.md", "docs/架构设计.md", "docs/网络编程.md", "docs/数据库访问.md",
    "docs/部署与运维.md", "docs/配置与可观测性.md", "docs/测试与验证.md", "docs/性能测试.md",
)
# 提取数下限：正则失配、扫描清单被改名、台账整片不见都会让提取数塌下来而门照绿。
# 本档取九张 Mermaid 图进正文之后实测的提取数 414 往下留一成（414 - 414 // 10 = 373）；引用面再涨时
# 按当时的同一实测重钉。塌到地板以下是「判据没吃到东西」的哨兵（退 2），不是引用质量下降的记分牌。
MINIMUM_TOTAL_PROSE_REFS = 373


def classify_line(lines, number):
    """返回 (判定, 内容)。number 是 1 基行号。"""
    if number > len(lines):
        return "BEYOND_EOF", ""
    content = lines[number - 1].strip()
    if not content:
        return "BLANK", ""
    if content.startswith(COMMENT_PREFIXES):
        return "COMMENT", content
    return "OK", content


def tracked_file_index(root):
    """文件名 → 仓库内路径列表，用来把正文里只写文件名的引用对上真文件。git 不可用时回 None。"""
    try:
        result = subprocess.run(["git", "ls-files"], cwd=root, capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    index = {}
    for line in result.stdout.splitlines():
        rel = line.strip().replace("\\", "/")
        if rel:
            index.setdefault(os.path.basename(rel), []).append(rel)
    return index


def prose_ref_path(match, owner, index, root):
    """把一条正文引用对到仓库内路径。写全路径直接认；只写文件名时按仓库内唯一同名文件补全，
    重名就不猜（猜错会把「引用有效」报给一个根本没被核对的行）。"""
    if match.group("path"):
        rel = match.group("path")
        if os.path.isfile(os.path.join(root, rel)):
            return rel
        if index is None:
            return None
        hits = index.get(os.path.basename(rel), [])
        return hits[0] if len(hits) == 1 else None
    if match.group("ext"):
        if owner is None or index is None:
            return None
        want = os.path.basename(owner).rsplit(".", 1)[0] + match.group("ext")
        for candidate in index.get(want, []):
            if os.path.dirname(candidate) == os.path.dirname(owner):
                return candidate
        return None
    return owner


def prose_ref_line(match):
    for key in ("full", "suffixed", "colon", "comma"):
        if match.group(key):
            return int(match.group(key))
    return None


def scan_prose_text(trail, text, index, line_cache, root, allow_external_bare_names=False):
    """对一段正文跑「路径:行号」判据，返回 (已核对条数, FAIL 列表, WARN 列表)。

    trail 是这段正文在台账里的位置（进消息用）。allow_external_bare_names 为真时，裸文件名
    对不到树内文件按仓库外依赖头记 WARN，写全仓相对路径读不到仍一律 FAIL。
    """
    failures = []
    warnings = []
    matchedCount = 0
    owner = None
    for match in PROSE_REF_RE.finditer(text):
        line = prose_ref_line(match)
        rel = prose_ref_path(match, owner, index, root)
        if match.group("path") and rel:
            owner = rel
        if rel is None or line is None:
            if match.group("path"):
                if allow_external_bare_names and "/" not in match.group("path"):
                    warnings.append("REF_OUTSIDE_TREE %s -> %s（裸文件名且树内无同名文件：按仓库外引用跳过）" % (trail, match.group("path")))
                else:
                    failures.append("PROSE_MISSING_PATH %s -> %s（正文点名的文件不在树里）" % (trail, match.group("path")))
            # 只写了 :123 或 、123 而前面没有完整路径：续写没有可继承的对象，这条压根
            # 无法定位。判红等于逼人重写文案，所以略过——但它不等于「已核对过」
            continue
        matchedCount += 1
        if rel not in line_cache:
            absolute = os.path.join(root, rel)
            with open(absolute, encoding="utf-8", errors="replace") as handle:
                line_cache[rel] = handle.read().splitlines()
        lines = line_cache[rel]
        cited = "%s:%d" % (rel, line)
        verdict, _ = classify_line(lines, line)
        if verdict == "BEYOND_EOF":
            failures.append("PROSE_LINE_BEYOND_EOF %s -> %s（文件只有 %d 行）" % (trail, cited, len(lines)))
        elif verdict == "BLANK":
            failures.append("PROSE_LINE_BLANK %s -> %s（引到空行，等于没有证据）" % (trail, cited))
        elif verdict == "COMMENT":
            warnings.append("PROSE_LINE_ON_COMMENT %s -> %s" % (trail, cited))
        range_match = PROSE_RANGE_RE.match(text[match.end():])
        if range_match is None:
            continue
        end_line = int(range_match.group("dash") or range_match.group("dots"))
        end_verdict, _ = classify_line(lines, end_line)
        if end_verdict == "BEYOND_EOF":
            failures.append("PROSE_END_LINE_BEYOND_EOF %s -> %s..%d" % (trail, cited, end_line))
        elif end_line < line:
            failures.append("PROSE_END_LINE_BEFORE_START %s -> %s..%d" % (trail, cited, end_line))
    return matchedCount, failures, warnings


def read_ledger_prose_texts(root):
    """读 LEDGER_PROSE_FILES 中存在的每一份，返回 (打开份数, [(位置, 文本), ...])。
    位置是「文件:行号」；CHANGELOG 只取最新发布段：首个 `## [` 行起、到下一个之前，
    产出的第一条往往不是第 1 行，所以份数不能拿位置尾的行号反推。"""
    texts = []
    openedFiles = 0
    for relative in LEDGER_PROSE_FILES:
        absolute = os.path.join(root, relative)
        if not os.path.isfile(absolute):
            continue  # 缺文件：不计进打开份数，整片读不到时由 main 的份数哨退 2
        with open(absolute, encoding="utf-8", errors="replace") as handle:
            lines = handle.read().splitlines()
        openedFiles += 1
        insideLatestRelease = relative != "CHANGELOG.md"
        for number, text in enumerate(lines, 1):
            if relative == "CHANGELOG.md":
                if text.startswith("## ["):
                    if insideLatestRelease:
                        break  # 最新发布段到此结束
                    insideLatestRelease = True
                if not insideLatestRelease:
                    continue
            texts.append(("%s:%d" % (relative, number), text))
    return openedFiles, texts


def main() -> int:
    parser = argparse.ArgumentParser(description="台账正文「路径:行号」引用的可追溯性检查")
    parser.add_argument("--root", default=REPO, help="待检仓库树根（夹具自测用）")
    parser.add_argument("--min-refs", type=int, default=MINIMUM_TOTAL_PROSE_REFS, help="正文提取数地板")
    args = parser.parse_args()
    root = os.path.abspath(args.root)

    line_cache: dict[str, list[str]] = {}
    failures: list[str] = []
    warnings: list[str] = []
    total_prose = 0
    index = tracked_file_index(root)
    openedFiles, proseTexts = read_ledger_prose_texts(root)
    for trail, text in proseTexts:
        matchedCount, newFailures, newWarnings = scan_prose_text(trail, text, index, line_cache, root, allow_external_bare_names=True)
        total_prose += matchedCount
        failures.extend(newFailures)
        warnings.extend(newWarnings)

    if openedFiles == 0:
        sys.stderr.write("台账清单里 %d 份文件一份都没读到：扫描面没了，按「门没跑成」退 2\n" % len(LEDGER_PROSE_FILES))
        return 2
    if total_prose < args.min_refs:
        sys.stderr.write("提取数 %d 低于地板 %d：判据没吃到东西，按「门没跑成」退 2\n" % (total_prose, args.min_refs))
        return 2
    for line in failures:
        print("VIOLATION " + line)
    for line in warnings:
        print("WARN " + line)
    print("TOTAL_PROSE_REFS=%d FAIL=%d WARN=%d" % (total_prose, len(failures), len(warnings)))
    if failures:
        print("ARCH_TRACE_GATE=FAIL")
        return 1
    print("ARCH_TRACE_GATE=OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
