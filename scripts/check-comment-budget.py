#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""注释预算门禁：注释块最多 3 行，注释与 docstring 最多 100 显示列。

用法：
    python scripts/check-comment-budget.py [--root PATH]

退出码：0 合规；1 有违规；2 扫描面为空或根目录不可读。
"""

from __future__ import annotations

import argparse
import ast
import io
import re
import sys
import tokenize
import unicodedata
from pathlib import Path

SKIP_DIRECTORY_NAMES = {".git", "build", ".fuzz", "__pycache__", ".idea"}
EXCLUDED_FILE_NAMES = {"conan_provider.cmake"}
MAX_COMMENT_BLOCK_LINES = 3
MAX_DISPLAY_COLUMNS = 100
MODULE_DOCSTRING_MAX_LINES = 10
OTHER_DOCSTRING_MAX_LINES = 4

DIRECTIVE_PATTERN = re.compile(
    r"^#\s*(?:(?:shellcheck|noqa|clang-format|cmake-format)\b|(?:type|pragma|pylint|mypy|isort|fmt):)"
)
CODING_PATTERN = re.compile(r"coding[:=]\s*[-\w.]+")
HEREDOC_WORD_PATTERN = re.compile(r"-?[ \t]*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")


def display_width(text: str) -> int:
    text = text.expandtabs(4)
    return sum(2 if unicodedata.east_asian_width(character) in ("W", "F") else 1 for character in text)


def is_exempt_comment(comment_text: str, line_number: int) -> bool:
    stripped = comment_text.lstrip()
    if stripped.startswith("#!"):
        return True
    if line_number <= 2 and CODING_PATTERN.search(stripped):
        return True
    return bool(DIRECTIVE_PATTERN.match(stripped))


def is_single_token_reference(comment_text: str) -> bool:
    body = comment_text.lstrip()[1:].strip()
    if not body or any(character.isspace() for character in body):
        return False
    return "://" in body or "/" in body or "\\" in body


def iter_source_files(root: Path) -> list[Path]:
    files = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if any(part in SKIP_DIRECTORY_NAMES for part in relative.parts[:-1]):
            continue
        if path.name in EXCLUDED_FILE_NAMES:
            continue
        if path.name == "CMakeLists.txt" or path.suffix in {".cmake", ".py", ".sh"}:
            files.append(path)
    return sorted(files)


def shell_comment_index(line: str) -> int | None:
    quote: str | None = None
    escaped = False
    for index, character in enumerate(line):
        if escaped:
            escaped = False
            continue
        if character == "\\" and quote != "'":
            escaped = True
            continue
        if quote:
            if character == quote:
                quote = None
            continue
        if character in ("'", '"'):
            quote = character
            continue
        if character == "#" and (index == 0 or line[index - 1].isspace()):
            return index
    return None


def shell_heredoc_terminator(line: str, comment_index: int | None) -> str | None:
    limit = len(line) if comment_index is None else comment_index
    quote: str | None = None
    arithmetic_depth = 0
    index = 0
    while index < limit:
        character = line[index]
        if quote is not None:
            if character == "\\" and quote == '"':
                index += 2
                continue
            if character == quote:
                quote = None
            index += 1
            continue
        if character == "\\":
            index += 2
            continue
        if character in ("'", '"'):
            quote = character
            index += 1
            continue
        if arithmetic_depth:
            if character == "(":
                arithmetic_depth += 1
            elif character == ")":
                arithmetic_depth -= 1
            index += 1
            continue
        if line.startswith("$((", index):
            arithmetic_depth = 2
            index += 3
            continue
        if line.startswith("((", index):
            arithmetic_depth = 2
            index += 2
            continue
        if line.startswith("<<", index) and (index == 0 or line[index - 1] != "<"):
            match = HEREDOC_WORD_PATTERN.match(line, index + 2, limit)
            if match:
                return match.group(2)
        index += 1
    return None


def cmake_comment_index(line: str) -> int | None:
    in_quote = False
    escaped = False
    for index, character in enumerate(line):
        if escaped:
            escaped = False
            continue
        if in_quote and character == "\\":
            escaped = True
            continue
        if character == '"':
            in_quote = not in_quote
            continue
        if character == "#" and not in_quote:
            return index
    return None


def check_python(path: Path, root: Path) -> list[str]:
    relative = path.relative_to(root).as_posix()
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except tokenize.TokenError as error:
        position = error.args[1] if len(error.args) > 1 else (1, 0)
        return [f"{relative}:{position[0]}: parse: {error.args[0]}"]
    except (IndentationError, SyntaxError) as error:
        return [f"{relative}:{getattr(error, 'lineno', 1)}: parse: {error}"]

    errors = []
    comments = {token.start[0]: token for token in tokens if token.type == tokenize.COMMENT}
    run = 0
    for line_number, line in enumerate(lines, 1):
        token = comments.get(line_number)
        if token is None:
            run = 0
            continue
        comment_only = line[: token.start[1]].strip() == ""
        if not comment_only:
            run = 0
            if display_width(token.string) > MAX_DISPLAY_COLUMNS and not is_single_token_reference(token.string):
                errors.append(f"{relative}:{line_number}: width: 注释超过 {MAX_DISPLAY_COLUMNS} 列")
            continue
        if is_exempt_comment(token.string, line_number):
            run = 0
            continue
        run += 1
        if run > MAX_COMMENT_BLOCK_LINES:
            errors.append(f"{relative}:{line_number}: block: 连续注释超过 {MAX_COMMENT_BLOCK_LINES} 行")
        if display_width(line) > MAX_DISPLAY_COLUMNS and not is_single_token_reference(token.string):
            errors.append(f"{relative}:{line_number}: width: 注释超过 {MAX_DISPLAY_COLUMNS} 列")

    try:
        tree = ast.parse(text)
    except SyntaxError as error:
        errors.append(f"{relative}:{error.lineno}: parse: {error.msg}")
        return errors

    for node in ast.walk(tree):
        if not isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            continue
        docstring = ast.get_docstring(node)
        if docstring is None:
            continue
        limit = MODULE_DOCSTRING_MAX_LINES if isinstance(node, ast.Module) else OTHER_DOCSTRING_MAX_LINES
        if len(docstring.splitlines()) > limit:
            errors.append(
                f"{relative}:{getattr(node, 'lineno', 1)}: docstring-lines: docstring 超过 {limit} 行"
            )
        docstring_node = None
        if (
            node.body
            and isinstance(node.body[0], ast.Expr)
            and isinstance(node.body[0].value, ast.Constant)
            and isinstance(node.body[0].value.value, str)
        ):
            docstring_node = node.body[0]
        if docstring_node is not None:
            for offset in range(docstring_node.lineno, docstring_node.end_lineno + 1):
                if offset <= len(lines) and display_width(lines[offset - 1]) > MAX_DISPLAY_COLUMNS:
                    errors.append(f"{relative}:{offset}: docstring-width: docstring 超过 {MAX_DISPLAY_COLUMNS} 列")
    return errors


def check_plain(path: Path, root: Path, language: str) -> list[str]:
    relative = path.relative_to(root).as_posix()
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    errors = []
    run = 0
    heredoc: str | None = None
    bracket_comment = False
    for line_number, line in enumerate(lines, 1):
        stripped = line.lstrip()
        if heredoc is not None:
            if stripped == heredoc:
                heredoc = None
            continue
        if language == "cmake" and bracket_comment:
            if line.strip() == "]]":
                bracket_comment = False
                run = 0
                continue
            run += 1
            if run > MAX_COMMENT_BLOCK_LINES:
                errors.append(f"{relative}:{line_number}: block: 连续注释超过 {MAX_COMMENT_BLOCK_LINES} 行")
            if display_width(line) > MAX_DISPLAY_COLUMNS:
                errors.append(f"{relative}:{line_number}: width: 注释超过 {MAX_DISPLAY_COLUMNS} 列")
            if "]]" in line:
                bracket_comment = False
                run = 0
            continue

        if language == "shell":
            comment_index = shell_comment_index(line)
            terminator = shell_heredoc_terminator(line, comment_index)
            if terminator is not None:
                heredoc = terminator
        else:
            comment_index = cmake_comment_index(line)

        if comment_index is None:
            run = 0
            continue

        comment_text = line[comment_index:]
        comment_only = line[:comment_index].strip() == ""
        if language == "cmake" and comment_text.startswith("#[[") and "]]" not in comment_text:
            bracket_comment = True
            run = 0
            if display_width(comment_text) > MAX_DISPLAY_COLUMNS:
                errors.append(f"{relative}:{line_number}: width: 注释超过 {MAX_DISPLAY_COLUMNS} 列")
            continue
        if not comment_only:
            run = 0
            if display_width(comment_text) > MAX_DISPLAY_COLUMNS and not is_single_token_reference(comment_text):
                errors.append(f"{relative}:{line_number}: width: 注释超过 {MAX_DISPLAY_COLUMNS} 列")
            continue
        if is_exempt_comment(comment_text, line_number):
            run = 0
            continue
        run += 1
        if run > MAX_COMMENT_BLOCK_LINES:
            errors.append(f"{relative}:{line_number}: block: 连续注释超过 {MAX_COMMENT_BLOCK_LINES} 行")
        if display_width(line) > MAX_DISPLAY_COLUMNS and not is_single_token_reference(comment_text):
            errors.append(f"{relative}:{line_number}: width: 注释超过 {MAX_DISPLAY_COLUMNS} 列")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="检查构建脚本的注释预算")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    try:
        root = args.root.resolve()
    except OSError as error:
        print(f"COMMENT_BUDGET_GATE=FAIL reason=bad-root error={error}")
        return 2
    if not root.is_dir():
        print(f"COMMENT_BUDGET_GATE=FAIL reason=bad-root root={root}")
        return 2

    try:
        files = iter_source_files(root)
    except OSError as error:
        print(f"COMMENT_BUDGET_GATE=FAIL reason=scan-error path={root} error={error}")
        return 2
    if not files:
        print("COMMENT_BUDGET_GATE=FAIL reason=no-files")
        return 2

    violations = []
    for path in files:
        try:
            if path.suffix == ".py":
                violations.extend(check_python(path, root))
            elif path.suffix == ".sh":
                violations.extend(check_plain(path, root, "shell"))
            else:
                violations.extend(check_plain(path, root, "cmake"))
        except OSError as error:
            print(f"COMMENT_BUDGET_GATE=FAIL reason=scan-error path={path} error={error}")
            return 2

    if violations:
        for violation in violations:
            print(violation)
        affected = {violation.split(":", 1)[0] for violation in violations}
        print(f"COMMENT_BUDGET_GATE=FAIL violations={len(violations)} files={len(affected)}")
        return 1
    print(f"COMMENT_BUDGET_GATE=OK files={len(files)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
