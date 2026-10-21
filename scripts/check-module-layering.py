#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""模块分层门：include 与 CMake 依赖只允许 Platform ← Base ← Core ← {Net, Database}。

判据：
  * `#include "<模块>/..."` 的目标模块必须在该模块允许的下游集合里；
  * `target_link_libraries(<自身> ...)` 只许出现允许的下游模块；
  * 扫不到 5 个模块、模块缺 CMakeLists、或文件数低于下限退 2；
  * 违反退 1；全过退 0。

用法：python scripts/check-module-layering.py [--root PATH] [--min-files N] [--min-modules N]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

MODULES = ("Platform", "Base", "Core", "Net", "Database")
ALLOWED = {
    "Platform": set(),
    "Base": {"Platform"},
    "Core": {"Platform", "Base"},
    "Net": {"Platform", "Base", "Core"},
    "Database": {"Platform", "Base", "Core"},
}
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".inl"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"((?:Platform|Base|Core|Net|Database)/[^"]+)"')
LINK_RE = re.compile(r"target_link_libraries\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)(.*?)\)", re.S)
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def module_of(path: Path, root: Path) -> str | None:
    try:
        relative = path.relative_to(root / "src")
    except ValueError:
        return None
    return relative.parts[0] if relative.parts and relative.parts[0] in MODULES else None


def scan(root: Path) -> tuple[list[str], dict[str, int], list[str]]:
    violations: list[str] = []
    stats = {"files": 0, "includes": 0, "links": 0}
    missing_cmake: list[str] = []
    modules_present = {entry.name for entry in (root / "src").iterdir() if entry.is_dir()} & set(MODULES)

    for path in sorted((root / "src").rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        owner = module_of(path, root)
        if owner is None:
            continue
        stats["files"] += 1
        for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            target_module = match.group(1).split("/", 1)[0]
            if target_module == owner:
                continue
            stats["includes"] += 1
            if target_module not in ALLOWED[owner]:
                violations.append(
                    f"{path.relative_to(root)}:{line_number} 包含了 {match.group(1)}"
                    f"（{owner} 只允许 {sorted(ALLOWED[owner])}）"
                )

    for module in MODULES:
        if module not in modules_present:
            continue
        cmake = root / "src" / module / "CMakeLists.txt"
        if not cmake.is_file():
            missing_cmake.append(module)
            continue
        text = cmake.read_text(encoding="utf-8", errors="replace")
        for match in LINK_RE.finditer(text):
            if match.group(1) != module:
                continue
            for token in TOKEN_RE.findall(match.group(2)):
                if token not in MODULES or token == module:
                    continue
                stats["links"] += 1
                if token not in ALLOWED[module]:
                    violations.append(
                        f"{cmake.relative_to(root)} 链接了 {token}（{module} 只允许 {sorted(ALLOWED[module])}）"
                    )

    stats["modules"] = len(modules_present)
    return violations, stats, missing_cmake


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--min-files", type=int, default=300)
    parser.add_argument("--min-modules", type=int, default=5)
    args = parser.parse_args()
    root = Path(args.root).resolve()

    if not (root / "src").is_dir():
        print(f"LAYERING_GATE=ERROR 找不到 {root / 'src'}", file=sys.stderr)
        return 2

    violations, stats, missing_cmake = scan(root)
    if missing_cmake:
        print(f"LAYERING_GATE=ERROR 模块 {','.join(missing_cmake)} 缺少 CMakeLists.txt", file=sys.stderr)
        return 2
    if stats["modules"] < args.min_modules:
        print(f"LAYERING_GATE=ERROR 只扫到 {stats['modules']} 个模块（下限 {args.min_modules}）", file=sys.stderr)
        return 2
    if stats["files"] < args.min_files:
        print(f"LAYERING_GATE=ERROR 只扫到 {stats['files']} 个源文件（下限 {args.min_files}）", file=sys.stderr)
        return 2

    if violations:
        print(f"LAYERING_GATE=FAIL violations={len(violations)}")
        for violation in violations:
            print(f"  {violation}")
        return 1

    print(
        "LAYERING_GATE=OK modules=%d files=%d includes=%d links=%d"
        % (stats["modules"], stats["files"], stats["includes"], stats["links"])
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
