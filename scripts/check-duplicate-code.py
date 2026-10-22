#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重复代码门：归一化后连续 8 行以上的相同窗口，只准减少不准新增。

判据：去注释、字符串/字符字面量折叠为占位、压缩空白、跳过空行/纯括号/预处理行；8 行窗口里
      至少 5 行有效，按 SHA-256 聚合；跨文件或同文件相距 20 行以上同哈希窗口算一簇；
      基线里没有的新簇判红；消失的簇提示跑 --write 收紧；同名局部助手只报告不判红。
用法：python scripts/check-duplicate-code.py [--root PATH] [--baseline PATH] [--write]
      [--min-files N]
退出码：0 通过；1 新簇；2 前置缺件。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".inl"}
WINDOW = 8
MIN_MEANINGFUL = 5
SKIP_LINE_RE = re.compile(r"^\s*(?:#|public:|private:|protected:|namespace\b|using\b)")
HELPER_RE = re.compile(r"^\s*(?:static\s+)?(?:inline\s+)?(?:[\w:<>,\s*&]+?)\s+([a-z][A-Za-z0-9_]*)\s*\(")
CONTROL_NAMES = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "catch",
    "sizeof",
    "alignof",
    "decltype",
    "static_assert",
    "operator",
}
LITERAL_ROW_RE = re.compile(
    r'^\s*\{?\s*(?:"<str>"|[-\d.]+)(?:\s*,\s*(?:"<str>"|[-\d.]+|true|false))*\s*\}?,?\s*$'
)


def looks_like_data(window: list[str]) -> bool:
    return sum(1 for line in window if LITERAL_ROW_RE.match(line)) >= len(window) - 1


def collapse_places(places: list[tuple[str, int]]) -> list[tuple[str, int]]:
    collapsed: list[tuple[str, int]] = []
    last_by_file: dict[str, int] = {}
    for relative, start in sorted(places):
        if relative in last_by_file and start < last_by_file[relative] + WINDOW:
            continue
        collapsed.append((relative, start))
        last_by_file[relative] = start
    return collapsed


def strip_comments(text: str) -> str:
    output: list[str] = []
    index = 0
    state = "code"
    length = len(text)
    while index < length:
        character = text[index]
        if state == "code":
            if text.startswith("//", index):
                state = "line"
                index += 2
                continue
            if text.startswith("/*", index):
                state = "block"
                index += 2
                continue
            if character == '"':
                state = "string"
            elif character == "'":
                state = "char"
            output.append(character)
            index += 1
        elif state == "line":
            if character == "\n":
                state = "code"
                output.append("\n")
            index += 1
        elif state == "block":
            if text.startswith("*/", index):
                state = "code"
                index += 2
            else:
                if character == "\n":
                    output.append("\n")
                index += 1
        else:
            output.append(character)
            if character == "\\" and index + 1 < length:
                output.append(text[index + 1])
                index += 2
                continue
            if (state == "string" and character == '"') or (state == "char" and character == "'"):
                state = "code"
            index += 1
    return "".join(output)


def normalize(text: str) -> list[str]:
    lines: list[str] = []
    for raw in strip_comments(text).splitlines():
        stripped = raw.strip()
        if not stripped or SKIP_LINE_RE.match(stripped):
            continue
        if re.fullmatch(r"[{}\[\]();,:]+", stripped):
            continue
        stripped = re.sub(r'"(?:\\.|[^"\\])*"', '"<str>"', stripped)
        stripped = re.sub(r"'(?:\\.|[^'\\])*'", "'<char>'", stripped)
        stripped = re.sub(r"\s+", " ", stripped)
        if len(stripped) >= 4:
            lines.append(stripped)
    return lines


def scan(root: Path) -> tuple[dict[str, dict], dict[str, set[str]], int]:
    occurrences: dict[str, list[tuple[str, int]]] = defaultdict(list)
    samples: dict[str, str] = {}
    helper_files: dict[str, set[str]] = defaultdict(set)
    files = 0
    for path in sorted((root / "src").rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(root).as_posix()
        if "/wepoll" in relative:
            continue
        files += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = normalize(text)
        for start in range(0, max(0, len(lines) - WINDOW + 1)):
            window = lines[start : start + WINDOW]
            if sum(1 for line in window if len(line) >= 4) < MIN_MEANINGFUL:
                continue
            if looks_like_data(window):
                continue
            if all(line.endswith(";") for line in window):
                continue
            digest = hashlib.sha256("\n".join(window).encode("utf-8")).hexdigest()[:16]
            occurrences[digest].append((relative, start + 1))
            samples.setdefault(digest, "\n".join(window))

        stripped_lines = strip_comments(text).splitlines()
        depth = 0
        anonymous_end = -1
        pending_anonymous_namespace = False
        for index, line in enumerate(stripped_lines):
            if anonymous_end < 0:
                if re.match(r"^\s*namespace\s*\{", line):
                    anonymous_end = depth + 1
                elif re.fullmatch(r"\s*namespace\s*", line):
                    pending_anonymous_namespace = True
                elif pending_anonymous_namespace and line.strip() == "{":
                    anonymous_end = depth + 1
                    pending_anonymous_namespace = False
            depth += line.count("{") - line.count("}")
            if anonymous_end >= 0 and depth >= anonymous_end:
                match = HELPER_RE.match(line)
                if match and match.group(1) not in CONTROL_NAMES:
                    stripped_line = line.strip()
                    next_line = stripped_lines[index + 1].strip() if index + 1 < len(stripped_lines) else ""
                    if not stripped_line.endswith(";") and ("{" in stripped_line or next_line.startswith("{")):
                        helper_files[match.group(1)].add(relative)
            if anonymous_end >= 0 and depth < anonymous_end:
                anonymous_end = -1

    candidates: dict[str, dict] = {}
    for digest, places in occurrences.items():
        collapsed = collapse_places(places)
        distinct_files = {place[0] for place in collapsed}
        keep = len(distinct_files) >= 2
        if not keep:
            starts = [place[1] for place in collapsed]
            keep = any(starts[index + 1] - starts[index] >= 20 for index in range(len(starts) - 1))
        if keep:
            candidates[digest] = {
                "files": sorted(distinct_files),
                "occurrences": len(collapsed),
                "places": collapsed,
                "sample": samples[digest],
            }

    groups: dict[tuple, list[tuple[str, dict]]] = defaultdict(list)
    for digest, data in candidates.items():
        base = min(start for _, start in data["places"])
        signature = tuple(sorted((relative, start - base) for relative, start in data["places"]))
        groups[(tuple(data["files"]), signature)].append((digest, data))

    clusters: dict[str, dict] = {}
    for members in groups.values():
        digest, data = min(members, key=lambda item: min(start for _, start in item[1]["places"]))
        clusters[digest] = {
            "files": data["files"],
            "occurrences": max(item[1]["occurrences"] for item in members),
            "places": data["places"],
            "sample": data["sample"],
        }
    helpers = {name: paths for name, paths in helper_files.items() if len(paths) >= 2 and len(name) >= 8}
    return clusters, helpers, files


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
        else Path(__file__).resolve().parent / "duplicate-code-baseline.json"
    )

    if not (root / "src").is_dir():
        print(f"DUPLICATE_CODE_GATE=ERROR 找不到 {root / 'src'}", file=sys.stderr)
        return 2

    clusters, helpers, files = scan(root)
    if files < args.min_files:
        print(f"DUPLICATE_CODE_GATE=ERROR 只扫到 {files} 个源文件（下限 {args.min_files}）", file=sys.stderr)
        return 2

    if args.write:
        payload = {
            "version": 1,
            "clusters": [
                {"hash": digest, "files": data["files"], "occurrences": data["occurrences"]}
                for digest, data in sorted(clusters.items())
            ],
        }
        baseline_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"DUPLICATE_CODE_GATE=WRITE files={files} clusters={len(clusters)}")
        return 0

    if not baseline_path.is_file():
        print(f"DUPLICATE_CODE_GATE=ERROR 读不到基线 {baseline_path}；先跑 --write 并在提交里说明", file=sys.stderr)
        return 2

    recorded = {
        entry["hash"] for entry in json.loads(baseline_path.read_text(encoding="utf-8")).get("clusters", [])
    }
    new = sorted(set(clusters) - recorded)
    stale = sorted(recorded - set(clusters))

    if helpers:
        print(f"NAMED_HELPERS={len(helpers)}（只报告，不判红）")
        for name, paths in sorted(helpers.items()):
            print(f"  {name}: {', '.join(sorted(paths))}")

    if new:
        print(f"DUPLICATE_CODE_GATE=FAIL files={files} new={len(new)}")
        for digest in new:
            data = clusters[digest]
            print(
                f"  新增重复 {digest} occurrences={data['occurrences']} "
                f"files={','.join(data['files'])} places={data['places'][:4]}"
            )
            for line in data.get("sample", "").splitlines()[:3]:
                print(f"    | {line[:100]}")
        return 1

    print(f"DUPLICATE_CODE_GATE=OK files={files} clusters={len(clusters)} new=0 stale={len(stale)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
