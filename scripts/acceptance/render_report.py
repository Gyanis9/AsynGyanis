"""把矩阵、执行结果、性能与环境事实渲染成一份 Markdown 报告。"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

import yaml

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")


def verdict_counts(rows: list[dict]) -> Counter:
    return Counter(row.get("status", "unverified") for row in rows)


def capability_table(rows: list[dict]) -> str:
    lines = ["| ID | 能力 | Runner | 状态 | 证据 |", "| --- | --- | --- | --- | --- |"]
    for row in rows:
        lines.append(f"| {row['id']} | {row['capability']} | {row['runner']} | {row['status']} | {row.get('evidence', '')} |")
    return "\n".join(lines)


def na_table(rows: list[dict]) -> str:
    selected = [row for row in rows if row.get("status") == "n-a"]
    if not selected:
        return "无 N/A 项。"
    lines = ["| ID | 能力 | 原因与补验条件 |", "| --- | --- | --- |"]
    for row in selected:
        lines.append(f"| {row['id']} | {row['capability']} | {row['defect']} |")
    return "\n".join(lines)


def defect_table(rows: list[dict]) -> str:
    selected = [row for row in rows if row.get("status") in {"red", "gap", "blocked"}]
    if not selected:
        return "无未结缺陷。"
    lines = ["| ID | 状态 | 原因 | 回归测试 |", "| --- | --- | --- | --- |"]
    for row in selected:
        lines.append(f"| {row['id']} | {row['status']} | {row.get('defect', '')} | {row.get('regression_test', '')} |")
    return "\n".join(lines)


def render(rows: list[dict], perf: dict, environment: dict) -> str:
    counts = verdict_counts(rows)
    parts = [
        "# 远端 ECS 全能力与性能验收报告",
        "",
        "## 判决汇总",
        "",
        f"- green：{counts.get('green', 0)}",
        f"- n-a：{counts.get('n-a', 0)}",
        f"- red：{counts.get('red', 0)}",
        f"- gap：{counts.get('gap', 0)}",
        f"- blocked：{counts.get('blocked', 0)}",
        f"- unverified：{counts.get('unverified', 0)}",
        "",
        "## 能力矩阵",
        "",
        capability_table(rows),
        "",
        "## 未结缺陷",
        "",
        defect_table(rows),
        "",
        "## N/A 与补验条件",
        "",
        na_table(rows),
        "",
        "## 性能",
        "",
        "```json",
        json.dumps(perf, ensure_ascii=False, indent=2),
        "```",
        "",
        "## 环境",
        "",
        "```json",
        json.dumps(environment, ensure_ascii=False, indent=2),
        "```",
        "",
    ]
    return "\n".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description="渲染验收报告")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--results", nargs="*", default=[])
    parser.add_argument("--perf", nargs="*", default=[])
    parser.add_argument("--environment", default="")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    rows = yaml.safe_load(Path(args.manifest).read_text(encoding="utf-8"))
    results = {}
    for path in args.results:
        for item in json.loads(Path(path).read_text(encoding="utf-8")):
            if item.get("status") == "dry-run":
                continue
            results[item["id"]] = item
    for row in rows:
        if row["id"] in results:
            row["status"] = results[row["id"]]["status"]
            if row["status"] == "red":
                row["defect"] = results[row["id"]].get("reason", row.get("defect", ""))
    perf = {}
    for path in args.perf:
        perf[Path(path).stem] = json.loads(Path(path).read_text(encoding="utf-8"))
    environment = json.loads(Path(args.environment).read_text(encoding="utf-8")) if args.environment else {}
    output = render(rows, perf, environment)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(output, encoding="utf-8")
    print(f"报告已生成：{args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
