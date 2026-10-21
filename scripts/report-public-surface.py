#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""公开面直测覆盖报告：列出公开声明里在 tests/ 中零引用的符号。

只报告不阻塞。按标识符出现计数，注释里的提及也算，因此「零引用」是强信号，
「有引用」仍需看断言是否真的覆盖行为。

用法：python scripts/report-public-surface.py [--root PATH] [--baseline PATH] [--out PATH]
退出码：0 正常产出；2 读不到基线。
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import Counter
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

KEYWORDS = {
    "class",
    "struct",
    "enum",
    "using",
    "typedef",
    "void",
    "bool",
    "int",
    "unsigned",
    "long",
    "short",
    "char",
    "float",
    "double",
    "const",
    "static",
    "inline",
    "virtual",
    "explicit",
    "noexcept",
    "override",
    "final",
    "return",
    "public",
    "private",
    "protected",
    "operator",
    "template",
    "typename",
    "namespace",
    "std",
    "size_t",
    "ssize_t",
    "uint8_t",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "int8_t",
    "int16_t",
    "int32_t",
    "int64_t",
    "string",
    "string_view",
    "vector",
    "optional",
    "expected",
    "span",
    "function",
    "chrono",
    "memory",
    "mutex",
    "atomic",
    "shared_ptr",
    "unique_ptr",
    "weak_ptr",
    "pair",
    "tuple",
    "array",
    "map",
    "unordered_map",
    "set",
    "unordered_set",
    "deque",
    "list",
    "byte",
    "error_code",
    "system_error",
    "exception",
    "stop_token",
    "jthread",
    "thread",
    "filesystem",
    "path",
}
TYPE_RE = re.compile(r"\b(?:class|struct|enum(?:\s+class)?|using|typedef)\s+([A-Za-z_][A-Za-z0-9_]*)")
CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def symbols_of(declaration: str) -> set[str]:
    found = set(TYPE_RE.findall(declaration)) | set(CALL_RE.findall(declaration))
    return {
        name
        for name in found
        if name not in KEYWORDS and len(name) > 2 and not re.fullmatch(r"[A-Z][A-Z0-9_]+", name)
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--baseline", default=None)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    baseline = Path(args.baseline) if args.baseline else root / "packaging" / "api-baseline.txt"

    if not baseline.is_file():
        print(f"PUBLIC_SURFACE_REPORT=ERROR 读不到 {baseline}", file=sys.stderr)
        return 2

    test_files = [
        path
        for path in sorted((root / "tests").rglob("*"))
        if path.is_file() and path.suffix in {".h", ".hpp", ".cpp", ".cc"}
    ]
    test_words: Counter[str] = Counter()
    for path in test_files:
        test_words.update(TOKEN_RE.findall(path.read_text(encoding="utf-8", errors="replace")))

    rows: list[tuple[str, str, int]] = []
    seen: set[tuple[str, str]] = set()
    for line in baseline.read_text(encoding="utf-8", errors="replace").splitlines():
        if "\t" not in line:
            continue
        header, declaration = line.split("\t", 1)
        for symbol in sorted(symbols_of(declaration)):
            key = (symbol, header)
            if key in seen:
                continue
            seen.add(key)
            rows.append((symbol, header, test_words.get(symbol, 0)))

    rows.sort(key=lambda row: (row[2], row[0], row[1]))
    if args.out:
        output_path = Path(args.out)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("symbol", "declarationFile", "testReferences"))
            writer.writerows(rows)
    else:
        writer = csv.writer(sys.stdout)
        writer.writerow(("symbol", "declarationFile", "testReferences"))
        writer.writerows(rows)

    zero = sum(1 for row in rows if row[2] == 0)
    print(
        f"PUBLIC_SURFACE_REPORT=OK symbols={len(rows)} zeroReferences={zero} testFiles={len(test_files)}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
