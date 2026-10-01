#!/usr/bin/env python3
"""覆盖率门禁：按 gcovr 的 CSV 逐模块判行覆盖，并判总数的函数/分支覆盖。

为什么不只判总行覆盖：总数 77% 达标时，Database 可以只有 50%——**聚合数会替死角掩护**。
这里把每个模块单独钉一条下限，并把函数/分支两条也判上（gcovr 的 CSV 一次给全，成本为零）。

下限的取法：全部按**本轮实测**往下留一档（实测见 --help 里那条注），不是随手拍的整数；
调高任何一条之前先实测一遍，两处必须同解。

用法：python scripts/check-coverage-gate.py --csv /tmp/gcovr.csv
退出码：0 达标；1 有模块或总数越界；2 输入不对（CSV 缺表、模块缺席、行数为零）
"""

from __future__ import annotations

import argparse
import csv
import io
import os
import sys
from collections import defaultdict

# 本轮（2026-10-01，CI 同配置：关 MySQL 驱动与示例、跑全量用例）实测：
#   TOTAL 行 77.08 / 函数 85.44 / 分支 62.38；Net 89.94、Platform 87.08、Core 84.98、Base 75.74、Database 50.50
DEFAULT_MODULE_FLOORS = "Net=85,Core=80,Platform=80,Base=70,Database=45"
# 聚合读数必须大到不像「筛错目录筛出个空表」：本仓 src/ 的可执行行是三万级
kMinimumTotalLines = 20000


def parseFloors(specification: str) -> dict:
    floors = {}
    for pair in specification.split(","):
        pair = pair.strip()
        if not pair:
            continue
        module, separator, value = pair.partition("=")
        if not separator:
            raise ValueError(f"模块下限要写成 模块=百分比，收到「{pair}」")
        floors[module.strip()] = float(value)
    return floors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", required=True, help="gcovr --csv 产出的文件")
    parser.add_argument("--line-floor", type=float, default=60.0, help="总行覆盖下限（保留原有那道）")
    parser.add_argument("--function-floor", type=float, default=75.0, help="总函数覆盖下限")
    parser.add_argument("--branch-floor", type=float, default=50.0, help="总分枝覆盖下限")
    parser.add_argument("--module-floors", default=DEFAULT_MODULE_FLOORS, help="形如 Net=85,Core=80 的模块行覆盖下限")
    arguments = parser.parse_args()

    try:
        moduleFloors = parseFloors(arguments.module_floors)
    except ValueError as error:
        print(f"模块下限写法不对：{error}")
        return 2

    try:
        with io.open(arguments.csv, encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
    except OSError as error:
        print(f"读不到 CSV（{arguments.csv}）：{error}")
        return 2

    if not rows or "filename" not in (rows[0] if rows else {}):
        print("CSV 是空的或没有表头——这道门按没跑成处理，不给「零失败即通过」")
        return 2

    totals = defaultdict(lambda: [0, 0, 0, 0, 0, 0])  # line_t, line_c, func_t, func_c, branch_t, branch_c
    for row in rows:
        filename = row["filename"].replace("\\", "/")
        if not filename.startswith("src/"):
            continue
        parts = filename.split("/")
        if len(parts) < 2:
            continue
        module = parts[1]
        bucket = totals[module]
        for index, key in enumerate(("line_total", "line_covered", "function_total", "function_covered",
                                     "branch_total", "branch_covered")):
            bucket[index] += int(row.get(key) or 0)

    if not totals:
        print("CSV 里一条 src/ 下的记录都没有：--filter 失配，这道门等于没跑")
        return 2

    failures = []
    grand = [0, 0, 0, 0, 0, 0]
    for module, bucket in totals.items():
        for index in range(6):
            grand[index] += bucket[index]

    def percent(numerator: int, denominator: int) -> float:
        return 0.0 if denominator <= 0 else 100.0 * numerator / denominator

    if grand[0] < kMinimumTotalLines:
        print(f"可执行行总数只有 {grand[0]}，低于 {kMinimumTotalLines} —— 读数不可信，按没跑成处理")
        return 2

    lines = []
    lines.append(("模块", "行", "行覆盖%", "下限", "判定"))
    for module in sorted(totals, key=lambda name: -percent(totals[name][1], totals[name][0])):
        bucket = totals[module]
        covered = percent(bucket[1], bucket[0])
        floor = moduleFloors.get(module)
        verdict = "—" if floor is None else ("OK" if covered >= floor else "低于下限")
        if floor is not None and covered < floor:
            failures.append(f"{module} 行覆盖 {covered:.2f}% 低于下限 {floor:.0f}%（共 {bucket[0]} 行）")
        lines.append((module, f"{bucket[0]}", f"{covered:.2f}", "—" if floor is None else f"{floor:.0f}%", verdict))
    for module in sorted(set(moduleFloors) - set(totals)):
        failures.append(f"模块 {module} 在 CSV 里一条记录都没有：它整个没被统计进去（目录改名或 filter 漂移）")

    totalLine = percent(grand[1], grand[0])
    totalFunction = percent(grand[3], grand[2])
    totalBranch = percent(grand[5], grand[4])
    for label, value, floor in (("行", totalLine, arguments.line_floor),
                                ("函数", totalFunction, arguments.function_floor),
                                ("分支", totalBranch, arguments.branch_floor)):
        if value < floor:
            failures.append(f"总{label}覆盖 {value:.2f}% 低于下限 {floor:.0f}%")

    print(f"{'模块':<10}{'行':>8}{'行覆盖%':>10}{'下限':>8}  判定")
    for row in lines[1:]:
        print(f"{row[0]:<10}{row[1]:>8}{row[2]:>10}{row[3]:>8}  {row[4]}")
    print(f"{'TOTAL':<10}{grand[0]:>8}{totalLine:>10.2f}{arguments.line_floor:>7.0f}%  "
          f"函数 {totalFunction:.2f}%（下限 {arguments.function_floor:.0f}%） 分支 {totalBranch:.2f}%（下限 {arguments.branch_floor:.0f}%）")

    summaryPath = os.environ.get("GITHUB_STEP_SUMMARY")
    if summaryPath:
        with io.open(summaryPath, "a", encoding="utf-8") as handle:
            handle.write("### 覆盖率分模块读数\n\n| 模块 | 行 | 行覆盖% | 下限 | 判定 |\n|---|---:|---:|---:|---|\n")
            for row in lines[1:]:
                handle.write("| " + " | ".join(row[:4]) + f" | {row[4]} |\n")
            handle.write(f"\n总：行 {totalLine:.2f}%（≥{arguments.line_floor:.0f}%）、函数 {totalFunction:.2f}%"
                         f"（≥{arguments.function_floor:.0f}%）、分支 {totalBranch:.2f}%（≥{arguments.branch_floor:.0f}%）\n")

    if failures:
        print("\n覆盖率门禁未过：")
        for line in failures:
            print("  - " + line)
        return 1
    print("覆盖率门禁通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
