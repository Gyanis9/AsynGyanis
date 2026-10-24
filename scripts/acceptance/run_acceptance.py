"""按能力矩阵执行单个波次，落盘证据并回写状态。"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import yaml

import manifest

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parents[2]


def wave_for(identifier: str, waves: dict[str, list[str]]) -> str | None:
    matches = [wave for wave, prefixes in waves.items() if any(identifier.startswith(prefix) for prefix in prefixes)]
    if len(matches) > 1:
        raise ValueError(f"{identifier} 必须至多命中一个波次，实际命中 {matches}")
    return matches[0] if matches else None


def shell_path(value: str | Path) -> str:
    return shlex.quote(Path(value).as_posix())


def render(template: str, args: SimpleNamespace, row: dict) -> str:
    return (
        template.replace("{repo}", shell_path(args.repo_root))
        .replace("{build}", shell_path(args.build))
        .replace("{python}", shell_path(args.python))
        .replace("{evidence}", shell_path(args.evidence_root / row["id"]))
        .replace("{server_pid}", str(getattr(args, "server_pid", 0)))
    )


def run_rows(rows: list[dict], args: SimpleNamespace) -> list[dict]:
    results: list[dict] = []
    for row in rows:
        command = render(row["command"], args, row)
        evidence_path = args.evidence_root / f"{row['id']}.log"
        declared_evidence = args.evidence_root / row["evidence"]
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        started = time.time()
        if getattr(args, "dry_run", False):
            print(f"[dry-run] {row['id']}: {command}")
            results.append({"id": row["id"], "status": "dry-run", "command": command, "reason": ""})
            continue
        bash = shutil.which("bash")
        invocation = [bash, "-lc", command] if bash else command
        try:
            completed = subprocess.run(
                invocation,
                shell=bash is None,
                cwd=args.repo_root,
                capture_output=True,
                text=True,
                timeout=args.timeout,
                check=False,
            )
            output = completed.stdout + completed.stderr
            evidence_path.write_text(output, encoding="utf-8")
            if declared_evidence != evidence_path:
                declared_evidence.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(evidence_path, declared_evidence)
            reason = f"exit={completed.returncode}" if completed.returncode else ""
            if not reason and "无 SKIP" in row["oracle"] and re.search(r"\[\s*SKIPPED\s*\]|\*{3}Skipped", output):
                reason = "判据要求无 SKIP，但日志包含跳过用例"
            results.append({"id": row["id"], "status": "red" if reason else "green", "command": command, "reason": reason, "seconds": time.time() - started})
        except subprocess.TimeoutExpired as expiry:
            output = expiry.stdout or ""
            errors = expiry.stderr or ""
            if isinstance(output, bytes):
                output = output.decode("utf-8", errors="replace")
            if isinstance(errors, bytes):
                errors = errors.decode("utf-8", errors="replace")
            evidence_path.write_text(output + errors, encoding="utf-8")
            results.append({"id": row["id"], "status": "red", "command": command, "reason": f"timeout={args.timeout}s", "seconds": time.time() - started})
    return results


def apply_results(rows: list[dict], results: list[dict]) -> None:
    by_id = {result["id"]: result for result in results}
    for row in rows:
        result = by_id.get(row["id"])
        if result is None or result["status"] == "dry-run":
            continue
        row["status"] = result["status"]
        if result["status"] == "red" and not row["defect"]:
            row["defect"] = result["reason"]


def main() -> int:
    parser = argparse.ArgumentParser(description="按能力矩阵执行验收波次")
    parser.add_argument("--manifest", default=str(REPO_ROOT / "tests" / "acceptance" / "capabilities.yaml"))
    parser.add_argument("--waves", default=str(REPO_ROOT / "tests" / "acceptance" / "waves.yaml"))
    parser.add_argument("--wave", default="", help="只跑某一波，如 w1")
    parser.add_argument("--only", default="", help="只跑这些 id，逗号分隔")
    parser.add_argument("--runner", default="ecs", choices=["ecs", "local-linux", "local-windows"])
    parser.add_argument("--build", default=str(REPO_ROOT / "build" / "release"))
    parser.add_argument("--evidence-root", default=str(REPO_ROOT.parent / "AsynGyanis-Reports" / "ecs-acceptance" / "runs"))
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--server-pid", type=int, default=0, help="性能长稳采样的服务进程号")
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--write-status", action="store_true")
    args = parser.parse_args()

    rows = manifest.load_manifest(Path(args.manifest))
    manifest.assert_valid(rows, REPO_ROOT)
    waves = yaml.safe_load(Path(args.waves).read_text(encoding="utf-8"))
    wanted = {item.strip() for item in args.only.split(",") if item.strip()}
    selected = []
    for row in rows:
        if row["runner"] != args.runner:
            continue
        if args.wave and wave_for(row["id"], waves) != args.wave:
            continue
        if wanted and row["id"] not in wanted:
            continue
        selected.append(row)
    if not selected:
        print("没有匹配的矩阵行", file=sys.stderr)
        return 2
    results = run_rows(
        selected,
        SimpleNamespace(
            repo_root=REPO_ROOT,
            evidence_root=Path(args.evidence_root),
            build=Path(args.build),
            python=args.python,
            server_pid=args.server_pid,
            timeout=args.timeout,
            dry_run=args.dry_run,
        ),
    )
    results_path = Path(args.evidence_root) / f"results-{args.wave or 'all'}.json"
    results_path.parent.mkdir(parents=True, exist_ok=True)
    results_path.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    if args.write_status and not args.dry_run:
        apply_results(rows, results)
        manifest.write_manifest(Path(args.manifest), rows)
    failures = [result for result in results if result["status"] == "red"]
    print(f"执行 {len(results)} 行，红 {len(failures)} 行；结果 {results_path}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
