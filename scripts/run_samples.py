#!/usr/bin/env python3
"""按模块跑完 samples/ 下的自检示例，汇总成一张矩阵并据此判断系统完整性。

示例自己报告结论：stdout 上打印 `RESULT <name> PASS|FAIL <步数> gated <跳过数>`，退出码 0 表示全绿。
本脚本不假设示例清单——它扫构建目录里现成的可执行文件，所以新增示例不需要改这里。
echo_server 是部署形态的服务器（不作自检），只按 --help 做一次冒烟运行。

    python scripts/run_samples.py                  # 跑全部
    python scripts/run_samples.py --only net_http_demo --repeat 3
    python scripts/run_samples.py --build build/release --timeout 300
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# 示例名前缀 → 所属模块；用于矩阵左侧的分组
MODULE_BY_PREFIX = {
    "base": "Base",
    "platform": "Platform",
    "core": "Core",
    "net": "Net",
    "database": "Database",
    # echo_server 是 Net 的部署形态示例，归到 Net 比落到 other 更看得出覆盖面
    "echo": "Net",
}

RESULT_LINE = re.compile(r"^RESULT\s+(\S+)\s+(PASS|FAIL)\s+(\d+)(?:\s+gated\s+(\d+))?\s*$", re.MULTILINE)

# 部署形态的服务器：它不是自检程序（直接跑会一直服务下去），只按 --help 冒烟一次
SMOKE_ONLY = {"echo_server"}


def infer_module(name: str) -> str:
    """按名字前缀猜模块名；猜不出时归到 other。"""
    prefix = name.split("_", 1)[0]
    return MODULE_BY_PREFIX.get(prefix, "other")


def find_sample_executables(build_dir: Path) -> list[Path]:
    """列出构建目录里的示例可执行文件。

    Windows（多配置生成器）与 Linux（单配置）都放在 samples/ 下，但 MSVC 也可能再套一层
    配置目录，因此两处都扫；只认「可执行形态」的文件名，跳过对象文件与中间产物。
    """
    candidates: list[Path] = []
    for samples_dir in (build_dir / "samples", build_dir / "samples" / "Debug"):
        if not samples_dir.is_dir():
            continue
        for entry in sorted(samples_dir.iterdir()):
            if not entry.is_file():
                continue
            is_windows_binary = entry.suffix == ".exe"
            is_posix_binary = entry.suffix == "" and entry.stat().st_mode & 0o111
            if is_windows_binary or is_posix_binary:
                candidates.append(entry)
    # 同名只留一份（先命中的samples/优先）
    unique: dict[str, Path] = {}
    for path in candidates:
        unique.setdefault(path.stem, path)
    return [unique[key] for key in sorted(unique)]


def targets_known_to_build(build_dir: Path) -> set[str] | None:
    """问构建系统「这一份配置到底产出哪些示例」；问不出来返回 None（退回按目录里有什么跑）。

    只按目录 glob 会把**已从构建清单里删掉或按平台关掉**的示例的残留可执行文件也算一份证据——那份
    二进制属于上一轮配置，跑它得出的红或绿都不属于当前这份代码（实测：Windows 上关掉的 core_upgrade
    留着一份 .exe，矩阵因此白红两条）。反过来，配置里有、目录里却没有的可执行文件也不能当没看见：
    那正是「示例根本没跑」的形态，门禁要出声。
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    makeProgram = ""
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_MAKE_PROGRAM:"):
            makeProgram = line.split("=", 1)[-1].strip()
            break
    # 只认 Ninja：Makefile 生成器下 make help 的目标名口径与产物对不上，问不准还不如不问
    ninja = Path(makeProgram.replace("\\", "/"))
    if "ninja" not in ninja.name.lower() or not ninja.is_file():
        return None
    listing = subprocess.run([str(ninja), "-C", str(build_dir), "-t", "targets", "all"],
                             capture_output=True, text=True, check=False)
    if listing.returncode != 0:
        return None
    names: set[str] = set()
    for line in listing.stdout.splitlines():
        target, _, kind = line.partition(":")
        target = target.replace("\\", "/")
        # 只认「直接落在 samples/ 下、由链接步骤产出」的那几行：同一份清单里还挂着 .obj、
        # install/edit_cache 之类的汇总目标，按名字收就会把「没产出可执行文件」报成一片假红
        if not target.startswith("samples/") or "/" in target[len("samples/"):]:
            continue
        if "EXECUTABLE" not in kind.upper():
            continue
        names.add(Path(target).stem)
    return names or None


def decode(raw: bytes) -> str:
    """日志在不同平台上可能是 UTF-8 也可能是 GBK；解不开就按替换字符返回。"""
    for encoding in ("utf-8", "gbk"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")


def run_one(executable: Path, timeout: float) -> tuple[str, str, int, float, str]:
    """跑一个示例，返回 (结论, 步数, 退出码, 用时秒, 失败时的末尾输出)。"""
    started = time.monotonic()
    arguments = ["--help"] if executable.stem in SMOKE_ONLY else []
    try:
        finished = subprocess.run([str(executable), *arguments], capture_output=True, text=False, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as expiry:
        # 超时时已经收着的输出也带上：挂死前跑到哪一步往往就在这几行里
        partial = decode(expiry.stdout) if isinstance(expiry.stdout, bytes) else str(expiry.stdout or "")
        return "TIMEOUT", "-", "-", -1, time.monotonic() - started, "\n".join(partial.splitlines()[-8:])

    elapsed = time.monotonic() - started
    output = decode(finished.stdout) + decode(finished.stderr)
    if executable.stem in SMOKE_ONLY:
        # 冒烟运行没有结论行：退出码为 0 就算通过（--help 会打印用法后正常返回）
        return ("PASS" if finished.returncode == 0 else f"FAIL(exit={finished.returncode})"), "-", "-", finished.returncode, elapsed, ""
    matches = RESULT_LINE.findall(output)
    if not matches:
        # 没有结论行：要么崩了，要么忘了调 finishSample——两种都算不合格
        tail = "\n".join(output.splitlines()[-5:])
        return f"NO_RESULT(exit={finished.returncode})", "-", "-", finished.returncode, elapsed, tail
    name, verdict, steps, gated = matches[-1]
    if verdict != "PASS":
        # 失败时带上末尾输出：在容器里跑时不必再单独复现一次才知道是哪一步红
        return "FAIL", steps, gated or "-", finished.returncode, elapsed, "\n".join(output.splitlines()[-8:])
    return "PASS", steps, gated or "-", finished.returncode, elapsed, ""


def main() -> int:
    parser = argparse.ArgumentParser(description="跑完 samples/ 下的自检示例并汇总")
    parser.add_argument("--build", default="build/debug", help="构建目录（默认 build/debug）")
    parser.add_argument("--timeout", type=float, default=180.0, help="单个示例的时限秒数")
    parser.add_argument("--only", default="", help="只跑这些示例，逗号分隔")
    parser.add_argument("--repeat", type=int, default=1, help="每个示例重复几遍（用来抓时序不稳的用例）")
    parser.add_argument("--list", action="store_true", help="只列出找到的示例，不运行")
    arguments = parser.parse_args()

    build_dir = (REPO_ROOT / arguments.build) if not Path(arguments.build).is_absolute() else Path(arguments.build)
    executables = find_sample_executables(build_dir)
    wanted = {name.strip() for name in arguments.only.split(",") if name.strip()}
    missingNames: list[str] = []
    known = targets_known_to_build(build_dir)
    if known:
        for name in sorted(path.stem for path in executables if path.stem not in known):
            print(f"  !! 跳过 {name}：这一份配置不产出它，磁盘上那份是上一轮构建留下的残留", file=sys.stderr)
        executables = [path for path in executables if path.stem in known]
        # 只对「本次要跑的这一批」查缺：--only 时其余目标本就没打算跑
        scope = wanted if wanted else known
        missingNames = sorted(scope - {path.stem for path in executables})
        for name in missingNames:
            print(f"  !! 配置里有示例 {name}，构建目录里却没有可执行文件——它这一次根本没跑", file=sys.stderr)
    if wanted:
        executables = [path for path in executables if path.stem in wanted]
    if not executables:
        print(f"在 {build_dir} 下没找到示例可执行文件——先构建 samples 目标", file=sys.stderr)
        return 2

    if arguments.list:
        for path in executables:
            print(f"{infer_module(path.stem):10s} {path.stem}")
        return 0

    rows: list[tuple[str, str, str, str, str, str]] = []
    failures = 0
    for path in executables:
        for _ in range(max(1, arguments.repeat)):
            verdict, steps, gated, code, elapsed, detail = run_one(path, arguments.timeout)
            rows.append((infer_module(path.stem), path.stem, verdict, str(steps), str(gated), f"{elapsed:.1f}s"))
            if verdict != "PASS":
                failures += 1
                print(f"  !! {path.stem} {verdict} 退出码 {code}", file=sys.stderr)
                if detail:
                    print(f"     末尾输出：\n       {detail}", file=sys.stderr)

    # 配置里存在、却没在磁盘上出现的示例：一条都不许默默过去（少跑一条比跑错一条更难发现）
    failures += len(missingNames)

    # --repeat 之间步数与门控步数都必须一致：某一步被条件跳过（平台分支、可选依赖缺席、初始化提前
    # return）时结论行仍是 PASS，只有步数会掉；真机门控步在几次运行里忽有忽无，说明环境本身不稳。
    # 把这两种抖动都当失败，才不至于让重复跑只用来抓崩溃
    if arguments.repeat > 1:
        evidenceByName: dict[str, set[str]] = {}
        for _module, name, verdict, steps, gated, _elapsed in rows:
            # 只比真正跑到结论的运行：TIMEOUT / NO_RESULT 那几行本来就已经计过失败了
            if verdict in ("PASS", "FAIL"):
                evidenceByName.setdefault(name, set()).add(f"{steps}+gated{gated}")
        for name, distinctEvidence in sorted(evidenceByName.items()):
            if len(distinctEvidence) > 1:
                failures += 1
                print(f"  !! {name} 在 {arguments.repeat} 次重复里证据形态不一致："
                      f"{'、'.join(sorted(distinctEvidence))}"
                      "（有步骤只在部分运行里执行或被门控跳过，PASS 不构成证据）", file=sys.stderr)

    module_width = max(len(row[0]) for row in rows) + 2
    name_width = max(len(row[1]) for row in rows) + 2
    print()
    print(f"{'模块'.ljust(module_width)}{'示例'.ljust(name_width)}{'结论':8s}{'步数':6s}{'门控':6s}用时")
    for module, name, verdict, steps, gated, elapsed in rows:
        print(f"{module.ljust(module_width)}{name.ljust(name_width)}{verdict:8s}{steps:6s}{gated:6s}{elapsed}")
    print(f"\n共 {len(rows)} 次运行，失败 {failures} 次")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
