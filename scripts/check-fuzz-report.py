#!/usr/bin/env python3
"""核对模糊测试这一轮的读数：各档解码器是不是**每档都被推到**、执行量够不够、有没有真跑完。

总执行数回答不了「其中一类根本没被走到」——混合跑里那类可能一条都没进。本脚本按
`scripts/fuzz-net.sh` 每类一份的日志逐类判，并把结果写成一张表进作业摘要。

用法：python scripts/check-fuzz-report.py <日志目录> [--targets A B C D] [--min-executions N]
退出码：0 全部达标；1 有目标没被推到 / 没跑完 / 执行量低于下限；2 参数或现场不对。
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

# libFuzzer 的收尾行形如 `#123456 DONE   cov: 789 ft: 1234 ...`；崩掉的运行里则是 `#123456 pulse` 之类
EXECUTIONS_DONE_PATTERN = re.compile(r"#(\d+)\s+DONE")
# 本仓的入口在退出前打的一行账：`FUZZ-TARGET-CALLS WebSocketFrame=12 Http2Frame=34 ...`
TARGET_CALLS_PATTERN = re.compile(r"FUZZ-TARGET-CALLS((?:\s+\w+=\d+)+)")
CRASH_PATTERN = re.compile(r"ERROR: (libFuzzer|AddressSanitizer)|Test unit written|SUMMARY: (?:lib)?Fuzzer")


def parseLog(path: Path) -> tuple:
    """返回 (执行次数, {目标: 次数}, 是否崩过, 是否跑完)"""
    text = path.read_text(encoding="utf-8", errors="replace")

    executions = 0
    for match in EXECUTIONS_DONE_PATTERN.finditer(text):
        executions = max(executions, int(match.group(1)))

    counts: dict[str, int] = {}
    for match in TARGET_CALLS_PATTERN.finditer(text):
        for pair in match.group(1).split():
            name, _, value = pair.partition("=")
            counts[name] = counts.get(name, 0) + int(value)

    return executions, counts, bool(CRASH_PATTERN.search(text)), bool(EXECUTIONS_DONE_PATTERN.search(text))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logDirectory", help="fuzz-net.sh 写日志的目录（.fuzz/log）")
    parser.add_argument("--targets", nargs="+",
                        default=["WebSocketFrame", "Http2Frame", "Http3Frame", "HpackBlock",
                                   "QuicPacket", "QuicFrameSequence", "QuicParameters"],
                        help="必须都被推到的目标名（与 ProtocolFuzzKernel.h 的 Target 同名）")
    parser.add_argument("--min-executions", type=int, default=1000,
                        help="每类的执行次数下限；只用来抓「根本没跑起来」，不拿机器快慢当回归判据")
    arguments = parser.parse_args()

    logDirectory = Path(arguments.logDirectory)
    if not logDirectory.is_dir():
        print(f"日志目录不存在：{logDirectory}")
        return 2

    rows = []
    failures = []
    seenNames: set[str] = set()
    for target in arguments.targets:
        path = logDirectory / (target + ".log")
        if not path.is_file():
            failures.append(f"{target}：没有 {path.name}，这一类根本没跑")
            rows.append((target, "-", "-", "-", "缺日志"))
            continue

        executions, counts, crashed, finished = parseLog(path)
        seenNames.update(counts)
        ownCount = counts.get(target, 0)
        others = {name: value for name, value in counts.items() if name != target}
        othersCalled = sum(others.values())
        verdict = "通过"
        if crashed:
            # 不只靠模糊器那头的退出码：-jobs 时父进程会不会把 worker 的退出码带出来没有保证，
            # 而「撞坏了」这件事在日志里是明写的。两道都判，宁可重复报红
            failures.append(f"{target}：日志里有崩溃/违例标记，制品在 .fuzz/artifacts/ 下")
            verdict = "发现违例"
        elif not finished:
            failures.append(f"{target}：日志里没有 DONE 收尾行，这一轮没跑完（看 {path.name}）")
            verdict = "没跑完"
        elif ownCount <= 0:
            failures.append(f"{target}：一类都没被推到（执行 {executions} 次却零调用）——选择目标的通路或枚举对不上")
            verdict = "零调用"
        elif othersCalled > 0:
            failures.append(f"{target}：指定了单目标却还解到别的类 {othersCalled} 次，说明每类独立预算没生效")
            verdict = "串了目标"
        elif executions < arguments.min_executions:
            failures.append(f"{target}：执行 {executions} 次，低于下限 {arguments.min_executions}")
            verdict = "量不足"
        rows.append((target, f"{executions:,}", f"{ownCount:,}", f"{othersCalled:,}", verdict))

    # 每一档的账都列全（含零调用的那些），所以「二进制里有、清单里没列」会在这里露头：
    # Target 加了新项而 CI 的默认清单没跟上时会判红，而不是让新目标静静没预算
    undeclared = sorted(seenNames - set(arguments.targets))
    if undeclared:
        failures.append("账上出现这些目标而 --targets 没列它们（Target 加了项、CI 清单没跟上）：" + " ".join(undeclared))

    total = sum(int(row[1].replace(",", "")) if row[1].replace(",", "").isdigit() else 0 for row in rows)
    # 「执行次数」取的是这一类里计数最大的那个 worker：-jobs=2 时它是总量的一半上下，
    # 用它当下限只会偏保守，不会把没跑起来的说成跑够了
    print("目标              执行次数*    本类调用    串到别类   判定")
    for target, executions, own, others, verdict in rows:
        print(f"{target:<16} {executions:>12} {own:>12} {others:>12}   {verdict}")
    print(f"合计执行 {total:,} 次（*按每类计数最大的 worker 计，多 worker 时是下界）")

    summaryPath = os.environ.get("GITHUB_STEP_SUMMARY")
    if summaryPath:
        with open(summaryPath, "a", encoding="utf-8") as handle:
            handle.write("### 模糊测试分目标读数\n\n")
            handle.write("| 目标 | 执行次数（每类计数最大的 worker，多 worker 时是下界） | 本类调用 | 串到别类 | 判定 |\n|---|---:|---:|---:|---|\n")
            for target, executions, own, others, verdict in rows:
                handle.write(f"| {target} | {executions} | {own} | {others} | {verdict} |\n")
            handle.write(f"\n合计执行 {total:,} 次；每类下限 {arguments.min_executions:,} 次。\n")

    if failures:
        print("\n".join("「" + line + "」" for line in failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
