#!/usr/bin/env python3
# 依赖公告台账的核对：conandata.yml 与 packaging/dependency-watch.json 必须同解。
#
# 这条门禁刻意**不**宣称「已核对无 CVE」——自动判定命中需要解析各家公告正文的格式，
# 猜错方向的后果是「绿着漏掉一条真漏洞」，比不跑更糟。它钉的是三件可机器判定的事：
#   1) 每条依赖都在台账里登记了公告入口（新增依赖忘了登记 → 红）；
#   2) 台账里的 pinnedVersion 与 conandata.yml 一致（升了版本没复核 → 红）；
#   3) 台账里没有已消失的依赖（改名/移除没同步 → 红）。
# 加 --fetch 时另做一次「公告入口链接还活着」的连通性核对；网络不通只提示，不判红——
# 上游页面挂掉不是本仓库的缺陷，但也不能因此把前三条一起跳过。

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

    for problem in problems:
        print(f"错误：{problem}", file=sys.stderr)
    if problems:
        print(f"共 {len(problems)} 项不一致；台账与清单必须同解，别只改一边", file=sys.stderr)
        return 1

    print(f"台账与清单同解：{len(requirements)} 条依赖，每条都有 https 公告入口")
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
