"""组装 ECS Release 包：构建树、依赖、夹具、脚本与校验和。

包的绝对路径按部署前缀保持不变：容器里在 /opt/asyngyanis-acceptance 构建，
ECS 也解包到同一路径，ctest 与 worker 夹具里编译进去的路径才成立。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


def libraries_of(ldd_output: str) -> list[str]:
    libraries: list[str] = []
    for line in ldd_output.splitlines():
        if "=>" not in line:
            continue
        target = line.split("=>", 1)[1].strip().split(" ", 1)[0]
        if target.startswith("/"):
            libraries.append(target)
    return libraries


def write_checksums(root: Path) -> None:
    lines = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        if path.name == "SHA256SUMS":
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f"{digest}  {path.relative_to(root).as_posix()}")
    (root / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")


def bundle_manifest(commit: str, build_type: str, flags: list[str], files: list[str]) -> dict:
    return {"commit": commit, "buildType": build_type, "flags": flags, "files": files}


CTEST_METADATA_PATTERNS = ("*_include.cmake", "*_tests.cmake")


def is_runtime_file(path: Path) -> bool:
    return path.is_file() and (path.suffix == "" or path.suffix == ".exe")


def should_bundle_library(path: str) -> bool:
    return not path.startswith(("/lib", "/usr/lib", "/lib64"))


def copy_file(source: Path, target: Path, copied: list[str], bundle: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    copied.append(target.relative_to(bundle).as_posix())


def main() -> int:
    parser = argparse.ArgumentParser(description="组装 Release 验收包")
    parser.add_argument("--build", default="build/release")
    parser.add_argument("--out", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    build = Path(args.build).resolve()
    bundle = Path(args.out)
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    copied: list[str] = []
    target_build = bundle / "build" / "release"

    for path in sorted(build.rglob("CTestTestfile.cmake")):
        copy_file(path, target_build / path.relative_to(build), copied, bundle)
    for pattern in CTEST_METADATA_PATTERNS:
        for path in sorted(build.rglob(pattern)):
            copy_file(path, target_build / path.relative_to(build), copied, bundle)
    for directory in ("tests", "samples", "benchmarks/microbench"):
        source = build / directory
        if not source.is_dir():
            continue
        for path in source.rglob("*"):
            if is_runtime_file(path):
                copy_file(path, target_build / path.relative_to(build), copied, bundle)

    fixtures = Path("tests/Core/fixtures")
    if fixtures.is_dir():
        for destination in (bundle / "fixtures", bundle / "tests" / "Core" / "fixtures"):
            if destination.exists():
                shutil.rmtree(destination)
            shutil.copytree(fixtures, destination)
            copied.extend(path.relative_to(bundle).as_posix() for path in destination.rglob("*") if path.is_file())

    for directory in ("scripts", "benchmarks", "packaging"):
        source = Path(directory)
        if source.is_dir():
            target = bundle / directory
            if target.exists():
                shutil.rmtree(target)
            shutil.copytree(source, target, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "build"))
            copied.extend(path.relative_to(bundle).as_posix() for path in target.rglob("*") if path.is_file())

    docs = Path("docs")
    if docs.is_dir():
        target = bundle / "docs"
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(docs, target, ignore=shutil.ignore_patterns("*.txt"))
        copied.extend(path.relative_to(bundle).as_posix() for path in target.rglob("*") if path.is_file())
    if Path("README.md").is_file():
        copy_file(Path("README.md"), bundle / "README.md", copied, bundle)

    for source in (Path("samples/CMakeLists.txt"), Path("tests/Tools/CMakeLists.txt")):
        if source.is_file():
            copy_file(source, bundle / source, copied, bundle)
    for name in ("capabilities.yaml", "waves.yaml", "perf-plan.yaml"):
        source = Path("tests/acceptance") / name
        if source.is_file():
            copy_file(source, bundle / "tests" / "acceptance" / name, copied, bundle)

    for name in copied:
        binary = bundle / name
        if not binary.is_file():
            continue
        try:
            output = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=False).stdout
        except FileNotFoundError:
            continue
        for library in libraries_of(output):
            if not should_bundle_library(library):
                continue
            target = bundle / "lib" / Path(library).name
            if not target.exists():
                copy_file(Path(library), target, copied, bundle)

    build_manifest = {
        "commit": args.commit,
        "buildType": "Release",
        "flags": ["-O3", "-DNDEBUG"],
        "deployPrefix": "/opt/asyngyanis-acceptance",
        "files": sorted(set(copied)),
    }
    (bundle / "bundle.json").write_text(json.dumps(build_manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    write_checksums(bundle)
    print(f"包已生成：{bundle}，{len(copied)} 个文件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
