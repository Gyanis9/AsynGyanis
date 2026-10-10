#!/usr/bin/env python3
"""加固位的核对：CMake 说它加了，与编译命令行、镜像头里真的带着，是三件不同的事。

判据的形状照本仓其它门禁来：

* **期望从构建目录自己的 CMakeCache.txt 现读**（`ASYN_ENABLE_HARDENING` / `ENABLE_SANITIZERS` /
  `SANITIZE_THREADS` / `CMAKE_BUILD_TYPE` / 编译器 id），脚本里不再抄一份策略——两处算式分叉是
  最难发现的那种漂移，而「配了却没生效」正是本仓在 sanitizer 上踩过的那类假安全；
* **编译侧**读 `compile_commands.json`：本仓 `src/` 下每个翻译单元都要带着该带的开关；
* **镜像侧**读产物头：ELF 看 `GNU_RELRO` + `BIND_NOW` + 不可执行栈，PE 看 NX / DYNAMIC_BASE /
  Control Flow Guard；
* 退出码分三档：0 全对、1 有落点不对（被测面的问题）、2 前置缺件（没有 compile_commands、找不到
  readelf/dumpbin 之类）——缺件绝不被读成「通过」。

    python3 scripts/check-hardening.py --build build/release
    python3 scripts/check-hardening.py --build build/debug --build build/release
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# 两个流固定按 UTF-8 写：这张表的每一行都是中文，而 Windows 侧的默认编码可能是 cp1252
# （GitHub 的 runner 实测就是，本机是 936 所以看不见）。按默认编码走，脚本会在**打印第一行读数时**
# 抛 UnicodeEncodeError 退 1——看上去像「加固落空」，实际一条判据都没跑完。
# 这条门第一次上 CI 就红在这里（2026-10-10，Linux 侧同一件跑过），所以补得不算早。
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ELF_FLAGS = {
    "stackProtector": "-fstack-protector-strong",
    "fortify": "-D_FORTIFY_SOURCE=2",
}
MSVC_FLAGS = {
    "cfg": "/guard:cf",
}


class GateError(Exception):
    """前置缺件：判据跑不成，按退出码 2 收口而不是交出一张空表。"""


def cache_value(cache_path: Path, key: str) -> str | None:
    """从 CMakeCache.txt 取一条 *STRING/BOOL* 值；键不存在返回 None。"""
    if not cache_path.is_file():
        raise GateError(f"找不到 ${cache_path}：这一档根本没配置过，不能当成「加固没问题」")
    pattern = re.compile(r"^%s:[A-Z]+=(.*)$" % re.escape(key))
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pattern.match(line.strip())
        if match:
            return match.group(1).strip()
    return None


def truthy(value: str | None) -> bool:
    return (value or "").upper() in {"ON", "TRUE", "1", "Y", "YES"}


def load_compile_commands(build_dir: Path) -> list[dict]:
    path = build_dir / "compile_commands.json"
    if not path.is_file():
        raise GateError(f"{path} 不存在：CMAKE_EXPORT_COMPILE_COMMANDS 没开，编译侧无从核对")
    entries = json.loads(path.read_text(encoding="utf-8"))
    if not entries:
        raise GateError(f"{path} 是空表：一张空清单不能读成「每个翻译单元都过了」")
    return entries


def command_of(entry: dict) -> str:
    if "command" in entry:
        return entry["command"]
    if "arguments" in entry:
        return " ".join(entry["arguments"])
    raise GateError(f"条目里没有 command/arguments：{entry.get('file')}")


def check_compile_side(expect: dict, entries: list[dict], args: argparse.Namespace) -> list[str]:
    """核对 src/ 下每个翻译单元带着该带的开关；返回落空的文件清单。

    加固不落的那一档反过来也要核：**一个都不该出现**——sanitizer 与加固叠在一起会让一次 trap
    归因不清，而「缓存里说关了、命令行里还带着」正是这种静默失效的形状。
    """
    problems: list[str] = []
    wanted: list[tuple[str, str]] = []
    forbidden: list[str] = list(ELF_FLAGS.values()) + list(MSVC_FLAGS.values())
    if expect["hardening"]:
        if expect["msvc"]:
            if expect["release"]:
                wanted.append(("cfg", MSVC_FLAGS["cfg"]))
            forbidden = []
        else:
            wanted.append(("stackProtector", ELF_FLAGS["stackProtector"]))
            if expect["release"]:
                wanted.append(("fortify", ELF_FLAGS["fortify"]))
            forbidden = [MSVC_FLAGS["cfg"]]
    else:
        # 编译器与档别决定的「本该有」在这里不作废：整组都不该出现（MSVC 那侧只有 CFG 一个词）
        forbidden = list(ELF_FLAGS.values()) + [MSVC_FLAGS["cfg"]]

    src_root = (expect["sourceRoot"] / "src").resolve()
    checked = 0
    for entry in entries:
        file_path = Path(entry["file"])
        try:
            file_path.resolve().relative_to(src_root)
        except ValueError:
            continue
        checked += 1
        command = command_of(entry)
        for _, flag in wanted:
            if flag not in command:
                problems.append(f"{file_path}：命令行里没有 {flag}")
        if not expect["hardening"]:
            present = [flag for flag in forbidden if flag in command]
            if present:
                problems.append(f"{file_path}：加固按缓存该整体失效，命令行里却带着 {present}")
    if checked == 0:
        raise GateError(f"编译数据库里没有 {src_root} 下的任何翻译单元：这一档根本没编本仓的代码")
    return problems


def run_tool(argv: list[str]) -> str:
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace")
    except OSError as error:
        raise GateError(f"调用 {' '.join(argv)} 失败：{error}") from error
    if proc.returncode != 0:
        raise GateError(f"{' '.join(argv)} 退 {proc.returncode}：{proc.stderr.strip()[:200]}")
    return proc.stdout + proc.stderr


def check_elf_binary(binary: Path, readelf: str) -> list[str]:
    """核 ELF 的三个镜像位：RELRO、BIND_NOW、不可执行栈。缺一个就是一条落空。"""
    problems: list[str] = []
    programHeaders = run_tool([readelf, "-lW", str(binary)])
    dynamic = run_tool([readelf, "-dW", str(binary)])
    if "GNU_RELRO" not in programHeaders:
        problems.append(f"{binary.name}：没有 GNU_RELRO 程序头（重定位后那段没被标成只读）")
    bindNow = "BIND_NOW" in dynamic or re.search(r"\(FLAGS\).*NOW", dynamic) is not None
    if not bindNow:
        problems.append(f"{binary.name}：没有 BIND_NOW（符号按需解析，GOT 在运行期仍可写）")
    stack = re.search(r"GNU_STACK\s+\S+\s+(\w+)\s", programHeaders)
    if stack is None:
        problems.append(f"{binary.name}：读不到 GNU_STACK 程序头，无法判定栈是否可执行")
    elif "E" in stack.group(1):
        problems.append(f"{binary.name}：栈段 flags 为 {stack.group(1)}，可执行栈")
    return problems


def check_pe_binary(binary: Path, dumpbin: str, expect: dict) -> list[str]:
    """核 PE 头的 DLL characteristics：NX 与 DYNAMIC_BASE 常开，CFG 只在加固且 Release 时才该在。"""
    problems: list[str] = []
    headers = run_tool([dumpbin, "/headers", str(binary)])
    # 特征位是十六进制且常以 A-F 开头（如 C160）：用 `\d+` 会从串中间截出一段数字，
    # 于是 NX 与 DYNAMIC_BASE 恰好落在那截里、看着「过了」，而高位（CFG 是 0x4000）整个被丢掉
    match = re.search(r"([0-9A-Fa-f]+)\s+DLL characteristics", headers)
    if match is None:
        return [f"{binary.name}：读不到 DLL characteristics，无法判定加固位"]
    characteristics = int(match.group(1), 16)
    if not characteristics & 0x100:  # IMAGE_DLLCHARACTERISTICS_NX_COMPAT
        problems.append(f"{binary.name}：没有 NX compatible（数据执行保护位没打开）")
    if not characteristics & 0x40:  # IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE
        problems.append(f"{binary.name}：没有 DYNAMIC_BASE（不 PIE，ASLR 对镜像基址无效）")
    if expect["release"] and not characteristics & 0x4000:  # IMAGE_DLLCHARACTERISTICS_GUARD_CF
        problems.append(f"{binary.name}：没有 Control Flow Guard（链接期没开 CFG）")
    return problems


def check_binary_side(build_dir: Path, expect: dict, args: argparse.Namespace) -> list[str]:
    """核镜像头。`--no-binaries` 给「只配置不构建」的那一档用（那里没有产物可读）。

    只抽前几件产物：同一棵树里所有产物出自同一份编译与链接选项，逐件读一遍除了耗时不增加判据；
    打印读了几件，免得被读成「整棵树都核过了」。
    """
    if not expect["hardening"]:
        return []
    if args.no_binaries:
        print("  （--no-binaries：这一档没有可链接产物，只核编译命令行）")
        return []
    explicit = [Path(item) for item in (args.binaries or [])]
    if explicit:
        binaries = explicit
    elif expect["msvc"]:
        binaries = pick(build_dir, [".exe"])[:2] + pick(build_dir, [".dll"])[:1]
    else:
        binaries = pick(build_dir, [""])[:2] + pick(build_dir, [".so"])[:1]
    if not binaries:
        raise GateError(f"{build_dir} 里找不到可核对的产物：这一档没链接出任何东西，镜像侧无从核对（别把「没核」读成「过了」）")
    print(f"  镜像侧核对 {len(binaries)} 件：{', '.join(path.name for path in binaries)}")
    problems: list[str] = []
    if expect["msvc"]:
        dumpbin = args.dumpbin or shutil.which("dumpbin")
        if not dumpbin:
            raise GateError("找不到 dumpbin：PE 头没法核对（在 vcvars 环境里跑，或显式 --no-binaries）")
        for binary in binaries:
            problems += check_pe_binary(binary, dumpbin, expect)
        return problems
    readelf = args.readelf or shutil.which("readelf") or shutil.which("llvm-readelf")
    if not readelf:
        raise GateError("找不到 readelf：ELF 头没法核对（别把「没核」读成「过了」）")
    for binary in binaries:
        problems += check_elf_binary(binary, readelf)
    return problems


def pick(build_dir: Path, suffixes: list[str]) -> list[Path]:
    picked: list[Path] = []
    for directory in ("tests", "samples", "benchmarks", "src"):
        root = build_dir / directory
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            if path.suffix in suffixes and os.access(path, os.X_OK):
                picked.append(path)
    return picked


def evaluate(build_dir: Path, args: argparse.Namespace) -> tuple[bool, list[str]]:
    cache = build_dir / "CMakeCache.txt"
    # 编译器 id 不是缓存项（它是配置期的普通变量），因此从缓存里真有的编译器路径判：
    # cl.exe 就是 MSVC 那一支，加固位是 /guard:cf；其余按 GCC/Clang 的 -f/-Wl 那一支
    compilerPath = cache_value(cache, "CMAKE_CXX_COMPILER") or ""
    isMsvc = Path(compilerPath).name.lower().startswith("cl") or "MSVC" in (cache_value(cache, "CMAKE_CXX_COMPILER_ID") or "")
    buildType = cache_value(cache, "CMAKE_BUILD_TYPE") or "Release"
    sourceRoot = cache_value(cache, "CMAKE_HOME_DIRECTORY") or str(build_dir.parent.parent)
    rawHardening = cache_value(cache, "ASYN_ENABLE_HARDENING")
    if rawHardening is None:
        raise GateError(f"{cache} 里没有 ASYN_ENABLE_HARDENING：这一档是在本开关之前配置的，"
                        "拿一张旧缓存判「加固不落」等于把没重新 configure 读成通过")
    sanitizersOn = truthy(cache_value(cache, "ENABLE_SANITIZERS")) or truthy(cache_value(cache, "SANITIZE_THREADS"))
    expect = {
        "hardening": truthy(rawHardening) and not sanitizersOn,
        "reasonOff": ("sanitizer 档自动不叠" if sanitizersOn else "ASYN_ENABLE_HARDENING=OFF"),
        "msvc": isMsvc,
        "release": buildType.lower() == "release",
        "sourceRoot": Path(sourceRoot.replace("\\", "/")),
    }
    print(f"[{build_dir.name}] 编译器={compilerPath or '未知'}{'（MSVC）' if isMsvc else ''} "
          f"配置={buildType} 加固={'落' if expect['hardening'] else '不落（' + expect['reasonOff'] + '）'}")
    entries = load_compile_commands(build_dir)
    problems = check_compile_side(expect, entries, args)
    problems += check_binary_side(build_dir, expect, args)
    return expect["hardening"], problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="append", required=True, help="构建目录，可重复")
    parser.add_argument("--readelf", help="ELF 头工具（默认读 PATH 上的 readelf/llvm-readelf）")
    parser.add_argument("--dumpbin", help="PE 头工具（默认读 PATH 上的 dumpbin）")
    parser.add_argument("--binaries", action="append", help="显式给出要核对的产物路径（可重复）")
    parser.add_argument("--no-binaries", action="store_true", help="只核编译命令行，不读镜像头")
    args = parser.parse_args()

    failures = 0
    for item in args.build:
        build_dir = Path(item)
        try:
            active, problems = evaluate(build_dir, args)
        except GateError as error:
            print(f"前置缺件：{error}")
            return 2
        if problems:
            failures += 1
            for problem in problems[:20]:
                print(f"  VIOLATION {problem}")
            if len(problems) > 20:
                print(f"  …另有 {len(problems) - 20} 处同样落空")
        else:
            print(f"  {'加固位齐' if active else '按预期不落加固位'}（编译命令行与镜像头都核对过）")

    if failures:
        print(f"HARDENING_GATE=FAIL builds={len(args.build)} 有 {failures} 档落空")
        return 1
    print(f"HARDENING_GATE=OK builds={len(args.build)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
