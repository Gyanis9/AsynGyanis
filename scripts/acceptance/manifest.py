"""能力矩阵的装载、字段校验与入口解析。"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

import yaml

REQUIRED_KEYS = (
    "id",
    "module",
    "capability",
    "claim_source",
    "platform",
    "runner",
    "prerequisites",
    "entry",
    "command",
    "oracle",
    "evidence",
    "status",
    "defect",
    "regression_test",
)
MODULES = {"Platform", "Base", "Core", "Net", "Database", "Deployment", "Performance"}
PLATFORMS = {"linux", "windows", "posix", "any"}
RUNNERS = {"ecs", "local-linux", "local-windows"}
STATUSES = {"unverified", "green", "red", "gap", "n-a", "blocked"}
ID_PATTERN = re.compile(r"^[A-Z][A-Z0-9]*(?:\.[A-Z0-9_]+)+$")
ENTRY_KINDS = {"test", "sample", "tool", "script", "manual"}


class ManifestError(ValueError):
    """矩阵校验失败时抛出的聚合错误。"""


def load_manifest(path: Path) -> list[dict[str, Any]]:
    data = yaml.safe_load(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise ManifestError(f"{path} 的顶层必须是列表")
    if not all(isinstance(item, dict) for item in data):
        raise ManifestError(f"{path} 的每一行必须是映射")
    return data


def write_manifest(path: Path, rows: list[dict[str, Any]]) -> None:
    lines = []
    for row in rows:
        dumped = yaml.safe_dump(
            row,
            allow_unicode=True,
            sort_keys=False,
            default_flow_style=True,
            width=100000,
        ).strip()
        lines.append(f"- {dumped}")
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def _sample_names(repo_root: Path) -> set[str]:
    text = (repo_root / "samples" / "CMakeLists.txt").read_text(encoding="utf-8")
    return set(re.findall(r"asyn_add_sample\((\w+)", text))


def _tool_names(repo_root: Path) -> set[str]:
    text = (repo_root / "tests" / "Tools" / "CMakeLists.txt").read_text(encoding="utf-8")
    return set(re.findall(r"add_executable\((\w+)", text))


def _entry_errors(value: Any, repo_root: Path) -> list[str]:
    if not isinstance(value, str) or ":" not in value:
        return [f"entry 必须是 test:/sample:/tool:/script:/manual: 之一，实际为 {value!r}"]
    kind, target = value.split(":", 1)
    if kind not in ENTRY_KINDS or not target:
        return [f"entry 的类别或目标为空：{value!r}"]
    if kind == "sample" and target not in _sample_names(repo_root):
        return [f"entry 指向不存在的 sample：{target}"]
    if kind == "tool" and target not in _tool_names(repo_root):
        return [f"entry 指向不存在的 tool：{target}"]
    if kind == "script" and not (repo_root / target).is_file():
        return [f"entry 指向不存在的 script：{target}"]
    return []


def validate_manifest(rows: list[dict[str, Any]], repo_root: Path) -> list[str]:
    errors: list[str] = []
    seen: set[str] = set()
    for index, row in enumerate(rows):
        prefix = f"第 {index + 1} 行"
        missing = [key for key in REQUIRED_KEYS if key not in row]
        extra = [key for key in row if key not in REQUIRED_KEYS]
        if missing:
            errors.append(f"{prefix} 缺字段：{', '.join(missing)}")
        if extra:
            errors.append(f"{prefix} 多出字段：{', '.join(extra)}")
        if missing:
            continue
        identifier = row["id"]
        if not isinstance(identifier, str) or not ID_PATTERN.fullmatch(identifier):
            errors.append(f"{prefix} 的 id 形状不合法：{identifier!r}")
        elif identifier in seen:
            errors.append(f"{prefix} 的 id 重复：{identifier}")
        else:
            seen.add(identifier)
        if row["module"] not in MODULES:
            errors.append(f"{prefix} 的 module 不合法：{row['module']!r}")
        if row["platform"] not in PLATFORMS:
            errors.append(f"{prefix} 的 platform 不合法：{row['platform']!r}")
        if row["runner"] not in RUNNERS:
            errors.append(f"{prefix} 的 runner 不合法：{row['runner']!r}")
        if row["status"] not in STATUSES:
            errors.append(f"{prefix} 的 status 不合法：{row['status']!r}")
        if not isinstance(row["prerequisites"], list):
            errors.append(f"{prefix} 的 prerequisites 必须是列表")
        for key in ("capability", "command", "oracle", "evidence"):
            if not isinstance(row[key], str) or not row[key].strip():
                errors.append(f"{prefix} 的 {key} 不能为空")
        claim = str(row["claim_source"]).split("#", 1)[0]
        if not (repo_root / claim).exists():
            errors.append(f"{prefix} 的 claim_source 不存在：{claim}")
        errors.extend(f"{prefix} {message}" for message in _entry_errors(row["entry"], repo_root))
        evidence = str(row["evidence"])
        if evidence.startswith("/") or ".." in Path(evidence).parts:
            errors.append(f"{prefix} 的 evidence 必须是仓库外报告目录内的相对路径：{evidence}")
        status = row["status"]
        if status == "green" and not str(row["evidence"]).strip():
            errors.append(f"{prefix} 的 green 行必须有 evidence")
        if status in {"red", "gap", "n-a", "blocked"} and not str(row["defect"]).strip():
            errors.append(f"{prefix} 的 {status} 行必须在 defect 字段写原因；n-a 还要写补验条件")
    return errors


def assert_valid(rows: list[dict[str, Any]], repo_root: Path) -> None:
    errors = validate_manifest(rows, repo_root)
    if errors:
        raise ManifestError("\n".join(errors))
