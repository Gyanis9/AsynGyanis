#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""架构工件可追溯性检查：图里每条 sources 引用都要在当场读得到，读不到就红。

判据的形状：
  * 引用路径不存在 → FAIL（改名或删文件留下的死引用，worker-handoff 那张就踩过一次）；
  * 行号越过文件末尾 → FAIL；引到空行 → FAIL（这一条最阴：文件还在、号还在，但那行什么都没有）；
  * 引到注释行或 doc-comment 起始 → WARN（本仓惯于引注释，不算坏，但要看得见有多少）；
  * 顶点类元素（components/nodes/states/participants）一条 sources 都没有 → WARN（图上的说法没有
    可回读的证据）；边与泳道不查——本仓的图里它们从不单独挂引用，硬要求只会造出噪声；
  * 新鲜度：按 meta.repository.revision 那一版做反向索引，被引文件自那之后改过就把受影响的
    节点列出来——这一条只列不拦，因为图落后于代码是常态，拦不住也不该由图来决定提交能不能合。

为什么是一道脚本而不是「记得改图」：383 笔提交里零次回图，靠人记住在这个变更速率下不成立。
判据必须自己会红。

用法：
    python3 scripts/check-arch-traceability.py                 # 检查（CI 用）
    python3 scripts/check-arch-traceability.py --stale-since HEAD~50   # 换新鲜度的基准
    python3 scripts/check-arch-traceability.py --strict         # 把 WARN 也算违例
退出码：0 通过；1 有违例；2 工件本身读不了（没有图、JSON 解不开、拿不到修订号）。
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIAGRAM_DIR = os.path.join(REPO, "assets", "diagrams")
# 只对「顶点类」集合要求证据：components/nodes/states/participants 每一格都是一句关于代码的断言，
# 而 edges/transitions/messages/lanes/phases/groups 在本仓的图里从不单独挂引用——把它们也算进
# 警告会一次产出 160 多行噪声，其结果就是没人再看这道门（实测过这个反面）。
VERTEX_COLLECTIONS = ("components", "nodes", "states", "participants")
# 注释行的形状：C/C++ 与 CMake/YAML/JSON/Python 的几种起始符。判据只用于「引到注释要不要警告」，
# 不参与任何 FAIL 判定，所以宁可写宽一点——把代码行误判成注释只会漏一条 WARN。
COMMENT_PREFIXES = ("//", "*", "/*", "#", ";;", "<!--")


def load_diagrams():
    """读回所有候选图 JSON。返回 (路径, 数据) 列表与一条错误说明——解不开就是工件坏了。"""
    names = sorted(n for n in os.listdir(DIAGRAM_DIR) if n.endswith(".json") and not n.endswith(".delivery.json"))
    found = []
    for name in names:
        path = os.path.join(DIAGRAM_DIR, name)
        try:
            with open(path, encoding="utf-8") as handle:
                found.append((name, json.load(handle)))
        except (ValueError, OSError) as error:
            return None, "读不了 %s：%s" % (name, error)
    return found, None


def iter_sources(node, trail, out):
    """穷举所有形如 {path, line[, end_line]} 的引用，连同它在图里的位置。"""
    if isinstance(node, dict):
        if "path" in node and "line" in node:
            out.append((trail, node))
        for key, value in node.items():
            iter_sources(value, "%s/%s" % (trail, key), out)
    elif isinstance(node, list):
        for index, value in enumerate(node):
            iter_sources(value, "%s[%d]" % (trail, index), out)


def iter_elements(data, name, out):
    """顶点类集合里每一格都该有证据；只查这几类（原因见 VERTEX_COLLECTIONS 的注释）。"""
    for collection in VERTEX_COLLECTIONS:
        elements = data.get(collection)
        if not isinstance(elements, list):
            continue
        for element in elements:
            if isinstance(element, dict) and "id" in element and "sources" not in element:
                out.append("%s -> %s/%s" % (name, collection, element.get("id")))


def classify_line(lines, number):
    """返回 (判定, 内容)。number 是 1 基行号。"""
    if number > len(lines):
        return "BEYOND_EOF", ""
    content = lines[number - 1].strip()
    if not content:
        return "BLANK", ""
    if content.startswith(COMMENT_PREFIXES):
        return "COMMENT", content
    return "OK", content


def git_changed_files(revision):
    """自 revision 起被改过的文件集合。拿不到就返回 None，让新鲜度这段明确报「没跑成」。"""
    try:
        result = subprocess.run(
            ["git", "diff", "--name-only", "%s..HEAD" % revision, "--", "src", "tests", "samples", "CMakeLists.txt", "cmake",
             "scripts", ".github", "README.md", "CONTRIBUTING.md", "packaging", "benchmarks"],
            cwd=REPO, capture_output=True, text=True, check=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        sys.stderr.write("新鲜度判定跳过：git 不可用或修订号取不到（%s）\n" % error)
        return None
    return {line.strip().replace("\\", "/") for line in result.stdout.splitlines() if line.strip()}


def main() -> int:
    parser = argparse.ArgumentParser(description="架构图 sources 引用的可追溯性检查")
    parser.add_argument("--stale-since", help="新鲜度基准修订号，默认取每张图 meta.repository.revision")
    parser.add_argument("--strict", action="store_true", help="把 WARN 也算违例")
    args = parser.parse_args()

    diagrams, load_error = load_diagrams()
    if load_error:
        sys.stderr.write(load_error + "\n")
        return 2
    if not diagrams:
        # 交不出任何待检对象不等于通过：目录被移动或改名时会静默变绿
        sys.stderr.write("找不到 %s 下的图 JSON——判据没跑，不是通过\n" % DIAGRAM_DIR)
        return 2

    line_cache = {}
    failures = []
    warnings = []
    total_refs = 0
    per_diagram_refs = {}
    cited_by_path = {}

    for name, data in diagrams:
        sources = []
        iter_sources(data, name, sources)
        per_diagram_refs[name] = len(sources)
        total_refs += len(sources)

        for trail, ref in sources:
            path = str(ref["path"]).replace("\\", "/")
            cited_by_path.setdefault(path, set()).add(trail)
            if path not in line_cache:
                absolute = os.path.join(REPO, path)
                if not os.path.isfile(absolute):
                    line_cache[path] = None
                else:
                    with open(absolute, encoding="utf-8", errors="replace") as handle:
                        line_cache[path] = handle.read().splitlines()
            lines = line_cache[path]
            if lines is None:
                failures.append("MISSING_PATH %s -> %s（引用路径不存在）" % (trail, path))
                continue
            verdict, _ = classify_line(lines, int(ref["line"]))
            if verdict == "BEYOND_EOF":
                failures.append("LINE_BEYOND_EOF %s -> %s:%d（文件只有 %d 行）" % (trail, path, int(ref["line"]), len(lines)))
            elif verdict == "BLANK":
                failures.append("LINE_BLANK %s -> %s:%d（引到空行，等于没有证据）" % (trail, path, int(ref["line"])))
            elif verdict == "COMMENT":
                warnings.append("LINE_ON_COMMENT %s -> %s:%d" % (trail, path, int(ref["line"])))
            end_line = ref.get("end_line")
            if end_line is not None:
                end_verdict, _ = classify_line(lines, int(end_line))
                if end_verdict == "BEYOND_EOF":
                    failures.append("END_LINE_BEYOND_EOF %s -> %s:%d" % (trail, path, int(end_line)))
                elif int(end_line) < int(ref["line"]):
                    failures.append("END_LINE_BEFORE_START %s -> %s:%d..%d" % (trail, path, int(ref["line"]), int(end_line)))

        bare = []
        iter_elements(data, name, bare)
        for element in bare:
            warnings.append("VERTEX_WITHOUT_SOURCES %s" % element)

    for name in sorted(per_diagram_refs):
        print("DIAGRAM %s refs=%d" % (name, per_diagram_refs[name]))

    revision = args.stale_since
    if revision is None:
        revisions = {str((data.get("meta") or {}).get("repository", {}).get("revision", "")) for _, data in diagrams}
        revisions.discard("")
        revision = sorted(revisions)[0] if len(revisions) == 1 else None
        if revision is None:
            sys.stderr.write("新鲜度判定跳过：各图钉的修订不止一个，请用 --stale-since 指定\n")
    if revision:
        changed = git_changed_files(revision)
        if changed is not None:
            stale_paths = sorted(p for p in cited_by_path if p in changed)
            affected = sorted({trail for path in stale_paths for trail in cited_by_path[path]})
            print("STALE_BASE=%s changed_cited_files=%d affected_refs=%d" % (revision[:8], len(stale_paths), len(affected)))
            for path in stale_paths[:15]:
                print("STALE_FILE %s（被 %d 处引用）" % (path, len(cited_by_path[path])))
            if len(stale_paths) > 15:
                print("STALE_FILE …另有 %d 个被引文件自该修订后变更" % (len(stale_paths) - 15))
            if affected:
                print("回图时按上面的引用位置逐条复核；这是清单不是判决")

    for line in failures:
        print("VIOLATION " + line)
    for line in warnings:
        print("WARN " + line)

    print("TOTAL_REFS=%d FAIL=%d WARN=%d" % (total_refs, len(failures), len(warnings)))
    if failures or (args.strict and warnings):
        print("ARCH_TRACE_GATE=FAIL")
        return 1
    print("ARCH_TRACE_GATE=OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
