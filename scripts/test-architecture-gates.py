#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""架构门自证：在 build/gate 下造临时树，证明每道门「种违规必红、撤掉必绿」。

用法：
    python scripts/test-architecture-gates.py                 # 跑全部
    python scripts/test-architecture-gates.py --only layering # 只跑一道

退出码：0 全过；1 有断言失败；2 夹具建不出来。
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = REPOSITORY_ROOT / "scripts"
FIXTURE_PARENT = REPOSITORY_ROOT / "build" / "gate" / "architecture-gate-tests"


def run_script(name: str, root: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPTS / name), "--root", str(root), *extra],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
    )


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def make_five_modules(root: Path) -> None:
    modules = ("Platform", "Base", "Core", "Net", "Database")
    allowed = {
        "Platform": "",
        "Base": "Platform",
        "Core": "Base\nPlatform",
        "Net": "Core\nBase\nPlatform",
        "Database": "Core\nBase\nPlatform",
    }
    for module in modules:
        write(root / "src" / module / f"{module}Type.h", "#pragma once\n")
        write(
            root / "src" / module / "CMakeLists.txt",
            "target_link_libraries(%s PUBLIC %s)\n" % (module, allowed[module].replace("\n", " ")),
        )


def expect(name: str, result: subprocess.CompletedProcess[str], expected_code: int, *needles: str) -> None:
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != expected_code:
        raise AssertionError(f"{name}: 期望退出码 {expected_code}，实际 {result.returncode}\n{output}")
    for needle in needles:
        if needle not in output:
            raise AssertionError(f"{name}: 输出里找不到 {needle!r}\n{output}")


def test_layering(root: Path) -> None:
    make_five_modules(root)
    write(root / "src" / "Base" / "UsesPlatform.h", '#pragma once\n#include "Platform/PlatformType.h"\n')
    write(root / "src" / "Core" / "UsesBase.h", '#pragma once\n#include "Base/UsesPlatform.h"\n')
    write(root / "src" / "Net" / "UsesCore.h", '#pragma once\n#include "Core/UsesBase.h"\n')
    write(root / "src" / "Database" / "UsesCore.h", '#pragma once\n#include "Core/UsesBase.h"\n')

    result = run_script("check-module-layering.py", root, "--min-files", "1")
    expect("layering 允许方向", result, 0, "LAYERING_GATE=OK")

    write(root / "src" / "Core" / "Reverse.h", '#pragma once\n#include "Net/UsesCore.h"\n')
    result = run_script("check-module-layering.py", root, "--min-files", "1")
    expect("layering 反向包含", result, 1, "Core", "Net/UsesCore.h")

    (root / "src" / "Core" / "Reverse.h").unlink()
    (root / "src" / "Database" / "CMakeLists.txt").write_text(
        "target_link_libraries(Database PUBLIC Net)\n", encoding="utf-8", newline="\n"
    )
    result = run_script("check-module-layering.py", root, "--min-files", "1")
    expect("layering 反向链接", result, 1, "Database", "Net")

    (root / "src" / "Database" / "CMakeLists.txt").unlink()
    result = run_script("check-module-layering.py", root, "--min-files", "1")
    expect("layering 模块缺 CMakeLists", result, 2, "模块")


def test_boundary(root: Path) -> None:
    make_five_modules(root)
    baseline = root / "baseline.json"
    write(root / "src" / "Core" / "bad.cpp", "#include <windows.h>\n#if _WIN32\nvoid f() { ::fork(); }\n#endif\n")

    result = run_script("check-platform-boundary.py", root, "--baseline", str(baseline), "--min-files", "1")
    expect("boundary 缺基线", result, 2, "基线")

    result = run_script("check-platform-boundary.py", root, "--baseline", str(baseline), "--min-files", "1", "--write")
    expect("boundary 重录", result, 0)

    result = run_script("check-platform-boundary.py", root, "--baseline", str(baseline), "--min-files", "1")
    expect("boundary 基线同解", result, 0, "PLATFORM_BOUNDARY_GATE=OK")

    write(root / "src" / "Net" / "bad2.cpp", "void g() { ::epoll_wait(0, nullptr, 0, 0); }\n")
    result = run_script("check-platform-boundary.py", root, "--baseline", str(baseline), "--min-files", "1")
    expect("boundary 新增违例", result, 1, "epoll_wait")

    (root / "src" / "Core" / "bad.cpp").unlink()
    result = run_script("check-platform-boundary.py", root, "--baseline", str(baseline), "--min-files", "1")
    expect("boundary 基线陈旧", result, 1, "收紧")

    result = run_script("check-platform-boundary.py", root, "--baseline", str(baseline), "--min-files", "1", "--write")
    expect("boundary 收紧后重录", result, 0)


def test_duplicates(root: Path) -> None:
    make_five_modules(root)
    baseline = root / "baseline.json"
    body = """int sharedWork(int value)
{
    int total = value;
    total += 1;
    total *= 2;
    total ^= value;
    total += 7;
    if (total > 10)
    {
        total -= 3;
    }
    return total;
}
"""
    write(root / "src" / "Core" / "a.cpp", body)
    write(root / "src" / "Net" / "b.cpp", body)

    result = run_script("check-duplicate-code.py", root, "--baseline", str(baseline), "--min-files", "1")
    expect("duplicates 缺基线", result, 2, "基线")

    result = run_script("check-duplicate-code.py", root, "--baseline", str(baseline), "--min-files", "1", "--write")
    expect("duplicates 重录", result, 0)

    result = run_script("check-duplicate-code.py", root, "--baseline", str(baseline), "--min-files", "1")
    expect("duplicates 基线同解", result, 0, "DUPLICATE_CODE_GATE=OK")

    second_body = """int anotherWork(int value)
{
    int result = value;
    result -= 4;
    result /= 2;
    result <<= 1;
    result |= value;
    while (result < 0)
    {
        result += 9;
    }
    return result;
}
"""
    write(root / "src" / "Base" / "c.cpp", second_body)
    write(root / "src" / "Database" / "d.cpp", second_body)
    result = run_script("check-duplicate-code.py", root, "--baseline", str(baseline), "--min-files", "1")
    expect("duplicates 新增簇", result, 1, "新增")


def test_surface(root: Path) -> None:
    write(
        root / "packaging" / "api-baseline.txt",
        "src/Net/Http/HttpServer.h\tclass HttpServer\nsrc/Net/Http/HttpServer.h\tvoid start()\n",
    )
    write(root / "tests" / "TestServer.cpp", "int main() { HttpServer server; server.start(); }\n")
    result = run_script("report-public-surface.py", root)
    expect("surface 报告", result, 0, "HttpServer")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", choices=("layering", "boundary", "duplicates", "surface"))
    args = parser.parse_args()

    FIXTURE_PARENT.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="gates-", dir=FIXTURE_PARENT))
    keep_fixture = False
    try:
        if args.only in (None, "layering"):
            test_layering(root / "layering")
        if args.only in (None, "boundary"):
            test_boundary(root / "boundary")
        if args.only in (None, "duplicates"):
            test_duplicates(root / "duplicates")
        if args.only in (None, "surface"):
            test_surface(root / "surface")
        print("ARCH_GATE_SELFTEST=OK")
        return 0
    except AssertionError as error:
        keep_fixture = True
        print(f"ARCH_GATE_SELFTEST=FAIL\n{error}", file=sys.stderr)
        print(f"夹具保留在 {root}", file=sys.stderr)
        return 1
    finally:
        if not keep_fixture:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
