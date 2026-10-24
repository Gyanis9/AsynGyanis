"""采集 ECS 机器事实、包校验和动态库解析结果。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_os_release(text: str) -> dict[str, str]:
    facts: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line or line.lstrip().startswith("#"):
            continue
        key, value = line.split("=", 1)
        facts[key.strip()] = value.strip().strip('"')
    return facts


def parse_meminfo(text: str) -> dict[str, int]:
    facts: dict[str, int] = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        fields = value.split()
        if not fields:
            continue
        facts[f"{key.strip()}Bytes"] = int(fields[0]) * 1024
    return facts


def verify_sha256(path: Path, expected: str) -> bool:
    digest = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    return digest.lower() == expected.lower()


def missing_libraries(ldd_output: str) -> list[str]:
    missing: list[str] = []
    for line in ldd_output.splitlines():
        if "=> not found" in line:
            missing.append(line.split("=>", 1)[0].strip())
    return missing


def run(command: list[str]) -> str:
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    return completed.stdout + completed.stderr


def io_uring_setup_allowed() -> bool:
    if platform.system() != "Linux":
        return False
    compiler = shutil.which("cc") or shutil.which("gcc")
    if compiler is None:
        return False
    source = r"""
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void) {
    struct io_uring_params params = {0};
    long result = syscall(__NR_io_uring_setup, 8, &params);
    return result >= 0 ? 0 : 1;
}
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "probe.c"
        binary = Path(directory) / "probe"
        path.write_text(source, encoding="ascii")
        built = subprocess.run([compiler, str(path), "-o", str(binary)], capture_output=True, check=False)
        if built.returncode != 0:
            return False
        return subprocess.run([str(binary)], capture_output=True, check=False).returncode == 0


def collect(bundle: Path) -> dict:
    resource = __import__("resource")
    facts: dict = {
        "uname": platform.uname()._asdict(),
        "python": sys.version,
        "cpuCount": os.cpu_count(),
        "ulimitNofile": resource.getrlimit(resource.RLIMIT_NOFILE),
        "ioUringSetupAllowed": io_uring_setup_allowed(),
    }
    os_release = Path("/etc/os-release")
    if os_release.is_file():
        facts["osRelease"] = parse_os_release(os_release.read_text(encoding="utf-8"))
    meminfo = Path("/proc/meminfo")
    if meminfo.is_file():
        facts["memory"] = parse_meminfo(meminfo.read_text(encoding="utf-8"))
    facts["disk"] = shutil.disk_usage(str(bundle.parent))
    facts["lddVersion"] = run(["ldd", "--version"]) if shutil.which("ldd") else "unavailable"
    checksums = bundle / "SHA256SUMS"
    if checksums.is_file():
        entries = []
        for line in checksums.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            expected, name = line.split(maxsplit=1)
            path = bundle / name.strip()
            entries.append({"name": name.strip(), "exists": path.is_file(), "sha256": expected})
        facts["checksums"] = entries
    binaries = sorted(path for path in bundle.rglob("*") if path.is_file() and os.access(path, os.X_OK))
    facts["ldd"] = []
    if shutil.which("ldd"):
        for binary in binaries:
            output = run(["ldd", str(binary)])
            facts["ldd"].append({"binary": str(binary.relative_to(bundle)), "missing": missing_libraries(output)})
    return facts


def main() -> int:
    parser = argparse.ArgumentParser(description="采集 ECS 环境事实")
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    facts = collect(Path(args.bundle))
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(facts, ensure_ascii=False, indent=2), encoding="utf-8")
    missing = [entry for entry in facts.get("ldd", []) if entry["missing"]]
    bad_checksums = [entry for entry in facts.get("checksums", []) if not entry["exists"]]
    if missing or bad_checksums:
        print(json.dumps({"missingLibraries": missing, "missingFiles": bad_checksums}, ensure_ascii=False, indent=2), file=sys.stderr)
        return 1
    print(json.dumps(facts, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
