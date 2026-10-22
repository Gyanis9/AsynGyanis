#!/usr/bin/env python3
"""依赖公告台账的核对：conandata.yml、packaging/dependency-watch.json、依赖表与交付配方同解。

不宣称「已核对无 CVE」——自动判定命中要解析各家公告正文，猜错的后果是绿着漏掉真漏洞。
机检五件事：每条依赖在台账登记公告入口；pinnedVersion 与 conandata.yml 一致；台账无已消失依赖；
docs/入门与构建.md 依赖表与清单逐行同解（库名与版本配对）；packaging/conan/conanfile.py 的
四处版本与清单同解。任一不一致退 1；--fetch 只核对链接可达性，网络不通只提示、不判红。
"""

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CONAN_DATA = REPO_ROOT / "conandata.yml"
WATCH_LIST = REPO_ROOT / "packaging" / "dependency-watch.json"
DEPENDENCY_GUIDE = REPO_ROOT / "docs" / "入门与构建.md"


def read_requirements() -> dict:
    """conandata.yml 的 requirements → {配方名: 版本}。"""
    if not CONAN_DATA.exists():
        raise SystemExit("缺少 conandata.yml")
    requirements = {}
    insideRequirements = False
    for line in CONAN_DATA.read_text(encoding="utf-8").splitlines():
        if re.match(r"^requirements\s*:\s*$", line):
            insideRequirements = True
            continue
        if insideRequirements and re.match(r"^\S", line):
            insideRequirements = False
        if insideRequirements:
            match = re.match(r"^\s*-\s*[\"']?([A-Za-z0-9_.+-]+)/([0-9][^\"'\s]*)[\"']?\s*$", line)
            if match:
                requirements[match.group(1)] = match.group(2)
    if not requirements:
        raise SystemExit("没解析出任何 requirements：清单格式变了，请同步改这两个脚本")
    return requirements


def read_watch_list() -> dict:
    """台账里的 dependencies 段。"""
    if not WATCH_LIST.exists():
        raise SystemExit(f"缺少依赖公告台账：{WATCH_LIST.relative_to(REPO_ROOT)}")
    document = json.loads(WATCH_LIST.read_text(encoding="utf-8"))
    dependencies = document.get("dependencies")
    if not isinstance(dependencies, dict) or not dependencies:
        raise SystemExit("台账里没有 dependencies 段，或它是空的")
    return dependencies


# docs/入门与构建.md 表里的展示名 → conandata.yml 的配方名：只列对不上的，其余按同名映射
DOCUMENT_NAME_ALIASES = {"OpenSSL": "openssl", "GoogleTest": "gtest", "SQLite3": "sqlite3"}


def read_dependency_table_pairs() -> dict:
    """docs/入门与构建.md「依赖版本」表 → {配方名: 版本}（一行多库时库名与版本都按 / 分隔）。"""
    if not DEPENDENCY_GUIDE.exists():
        raise SystemExit("缺少 DEPENDENCY_GUIDE.md")
    pairs = {}
    insideTable = False
    for line in DEPENDENCY_GUIDE.read_text(encoding="utf-8").splitlines():
        if not insideTable:
            if line.strip() == "| 库 | 版本 | 用途 |":
                insideTable = True
            continue
        # 表格在遇到第一行非表格行时结束；第一格是库名（带链接、一行多库用 / 分隔）、第二格是版本
        if not line.startswith("|"):
            break
        if set(line.strip()) <= set("|-: "):
            continue  # 表头下的分隔行（|---|------|------|）
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if len(cells) < 2:
            continue
        names = re.findall(r"\[([^\]]+)\]", cells[0]) or [name.strip() for name in cells[0].split("/") if name.strip()]
        versions = [version.strip() for version in cells[1].split("/") if version.strip()]
        if len(names) != len(versions):
            raise SystemExit(f"docs/入门与构建.md「依赖版本」表这一行的库名与版本条数对不上（{cells[0]} / {cells[1]}）：请同步改这个脚本或补齐表")
        for name, version in zip(names, versions):
            pairs[DOCUMENT_NAME_ALIASES.get(name, name)] = version
    if not pairs:
        raise SystemExit("docs/入门与构建.md 里没解析出依赖版本：表头或列序变了，请同步改这个脚本")
    return pairs


def check_document_versions(requirements: dict, documentPairs: dict) -> list:
    """docs/入门与构建.md 依赖表与 conandata.yml 逐行同解（库名与版本配对也要对上）。"""
    problems = []
    for name, version in sorted(requirements.items()):
        listed = documentPairs.get(name)
        if listed is None:
            problems.append(f"docs/入门与构建.md「依赖版本」表里没有 {name} 这一行（升版或新增依赖漏跟这张表）")
        elif listed != version:
            problems.append(f"docs/入门与构建.md「依赖版本」表里 {name} 写的是 {listed}，conandata.yml 是 {version}（表或清单有一边是旧的）")
    for name in sorted(set(documentPairs) - set(requirements)):
        problems.append(f"docs/入门与构建.md「依赖版本」表里的 {name} 不在 conandata.yml 里（改名或移除没同步）")
    return problems


def read_packaging_recipe_requirements() -> dict:
    """packaging/conan/conanfile.py 里写版本的四处 → {配方名: 版本}。

    四处＝BASE_REQUIREMENTS 列表与 JSON / YAML / MYSQL 三个 *_REQUIREMENT 常量。
    """
    recipe = REPO_ROOT / "packaging" / "conan" / "conanfile.py"
    if not recipe.exists():
        raise SystemExit("缺少 packaging/conan/conanfile.py")
    entries = {}
    insideBaseRequirements = False
    for line in recipe.read_text(encoding="utf-8").splitlines():
        if insideBaseRequirements:
            if line.startswith("]"):
                insideBaseRequirements = False
                continue
            match = re.match(r'^\s*"([A-Za-z0-9_.+-]+)/([0-9][^"\s]*)"\s*,?\s*$', line)
        else:
            if re.match(r"^BASE_REQUIREMENTS\s*=\s*\[", line):
                insideBaseRequirements = True
                continue
            match = re.match(r'^[A-Z_]+REQUIREMENT\s*=\s*"([A-Za-z0-9_.+-]+)/([0-9][^"\s]*)"\s*$', line)
        if match:
            entries[match.group(1)] = match.group(2)
    if not entries:
        raise SystemExit("交付配方里没解析出带版本的依赖：配方结构变了，请同步改这个脚本")
    return entries


def check_packaging_recipe_versions(requirements: dict, recipeRequirements: dict) -> list:
    """交付配方的依赖版本与 conandata.yml 同解（升版漏跟这一处，包就钉在旧版本上）。"""
    problems = []
    for name, version in sorted(recipeRequirements.items()):
        knownVersion = requirements.get(name)
        if knownVersion is None:
            problems.append(f"交付配方里的 {name}/{version} 没在 conandata.yml 里登记（新增依赖要一并补清单与公告台账）")
        elif knownVersion != version:
            problems.append(f"交付配方 packaging/conan/conanfile.py 里 {name} 钉的是 {version}，conandata.yml 是 {knownVersion}（升版要四处同改，否则交付包停在旧版）")
    return problems


def check_offline(requirements: dict, dependencies: dict) -> list:
    """前三条机器可判定规则，返回问题列表（空＝通过）。"""
    problems = []
    for name, version in sorted(requirements.items()):
        entry = dependencies.get(name)
        if entry is None:
            problems.append(f"{name}/{version} 没在台账里登记公告入口（新增依赖要一并补 packaging/dependency-watch.json）")
            continue
        pinned = entry.get("pinnedVersion", "")
        if pinned != version:
            problems.append(f"{name} 的台账版本 {pinned!r} 与 conandata.yml 的 {version!r} 不一致（升级没走复核）")
        advisories = entry.get("advisories") or []
        if not any(str(url).startswith("https://") for url in advisories):
            problems.append(f"{name} 的台账没有 https 公告入口：拿它查公告等于没查")
    for name in sorted(set(dependencies) - set(requirements)):
        problems.append(f"台账里的 {name} 已不在依赖清单中（依赖被移除或改名，台账要同步）")
    return problems


def check_links(dependencies: dict, timeoutSeconds: float) -> list:
    """--fetch：公告入口可达性。返回「不可达」提示列表，不参与退出码。"""
    notices = []
    for name, entry in sorted(dependencies.items()):
        for url in entry.get("advisories") or []:
            request = urllib.request.Request(url, headers={"User-Agent": "AsynGyanis-dependency-watch"})
            try:
                with urllib.request.urlopen(request, timeout=timeoutSeconds) as response:
                    if response.status >= 400:
                        notices.append(f"{name}: {url} 返回 HTTP {response.status}")
            except (urllib.error.URLError, TimeoutError, OSError) as error:
                notices.append(f"{name}: {url} 取不到（{type(error).__name__}）")
    return notices


def main() -> int:
    parser = argparse.ArgumentParser(description="核对依赖公告台账与固定版本是否同解")
    parser.add_argument("--fetch", action="store_true", help="额外核对公告入口链接可达性（不判红）")
    parser.add_argument("--timeout", type=float, default=20.0, help="单条链接的超时秒数")
    arguments = parser.parse_args()

    requirements = read_requirements()
    dependencies = read_watch_list()
    problems = check_offline(requirements, dependencies)
    problems += check_document_versions(requirements, read_dependency_table_pairs())
    problems += check_packaging_recipe_versions(requirements, read_packaging_recipe_requirements())

    for problem in problems:
        print(f"错误：{problem}", file=sys.stderr)
    if problems:
        print(f"共 {len(problems)} 项不一致；台账、清单、docs/入门与构建.md 依赖表与交付配方必须同解，别只改一边", file=sys.stderr)
        return 1

    print(f"台账、清单、docs/入门与构建.md 依赖表与交付配方同解：{len(requirements)} 条依赖，每条都有 https 公告入口")
    if arguments.fetch:
        notices = check_links(dependencies, arguments.timeout)
        if notices:
            print("提示（不影响门禁）：以下公告入口这次没取到，可能是不联网或上游临时不可用：")
            for notice in notices:
                print("  - " + notice)
        else:
            print("公告入口连通性：全部可达")
    return 0


if __name__ == "__main__":
    sys.exit(main())
