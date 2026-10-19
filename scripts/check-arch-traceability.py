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
    节点列出来——这一条只列不拦，因为图落后于代码是常态，拦不住也不该由图来决定提交能不能合；
  * 台账正文同判：`.github/SECURITY.md` 全篇与 `CHANGELOG.md` 的最新发布段里手写的「路径:行号」
    走同一组规则。更早的 CHANGELOG 段落是点时刻的叙述，不回判——历史条目随文件演进必然失效，
    硬判只会让这道门变成哭狼的门。写全仓相对路径而读不到 → FAIL；只写裸文件名（可能是仓库外的
    依赖头，如 zlib 的 zconf.h）而树里找不到同名文件 → WARN，不算死引用；
  * 交付 HTML 与源 JSON 同解：每张图旁挂的 `<slug>.html`（README 链接的交互版）内嵌一份
    source-evidence 块——修订号、引用总数、每元素的引用行位逐一与 JSON 比。两者由不同的手
    （改图 / 重出）推进，不同解就是「对外交付的是旧图」；HTML 缺失只记 WARN；
  * 空集防护：总提取数低于地板、或有图一条 sources 都没有 → 退 2。字段改名/结构变化会让提取数
    悄悄塌到 0 而门照绿，与覆盖率、复杂度两道门同一条规矩：「什么都没查」不许被读成通过。

为什么是一道脚本而不是「记得改图」：383 笔提交里零次回图，靠人记住在这个变更速率下不成立。
判据必须自己会红。

用法：
    python3 scripts/check-arch-traceability.py                 # 检查（CI 用）
    python3 scripts/check-arch-traceability.py --stale-since HEAD~50   # 换新鲜度的基准
退出码：0 通过；1 有违例；2 工件本身读不了（没有图、JSON 解不开、拿不到修订号）。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

# Windows runner 的控制台默认按 cp1252 编码：违例文案是中文，一 print 就抛 UnicodeEncodeError，
# 于是「红是红了，但没说是哪条引用」。与 check-ledger-dates.py 同形状，先钉两个流
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIAGRAM_DIR = os.path.join(REPO, "assets", "diagrams")
# 只对「顶点类」集合要求证据：components/nodes/states/participants 每一格都是一句关于代码的断言，
# 而 edges/transitions/messages/lanes/phases/groups 在本仓的图里从不单独挂引用——把它们也算进
# 警告会一次产出 160 多行噪声，其结果就是没人再看这道门（实测过这个反面）。
VERTEX_COLLECTIONS = ("components", "nodes", "states", "participants")
# 注释行的形状：C/C++ 与 CMake/YAML/JSON/Python 的几种起始符。判据只用于「引到注释要不要警告」，
# 不参与任何 FAIL 判定，所以宁可写宽一点——把代码行误判成注释只会漏一条 WARN。
COMMENT_PREFIXES = ("//", "*", "/*", "#", ";;", "<!--")
# 卡片正文与节点标签里也手写「路径:行号」，形状有四种（完整路径、只带后缀、单冒号续写、顿号续写），
# 后三种都从前一个完整路径继承文件。这一轮补它，是因为按出图那行的原文复核时发现 23 处号还在、
# 说的已经不是那件事——全部落在只查 sources 的判据射程之外。
SOURCE_EXTS = r"cpp|h|hpp|py|sh|json|ya?ml|md|txt"
PROSE_REF_RE = re.compile(
    rf"(?P<path>[A-Za-z0-9_./+-]+\.(?:{SOURCE_EXTS})):(?P<full>\d+)"
    rf"|(?P<ext>\.{SOURCE_EXTS}):(?P<suffixed>\d+)"
    rf"|(?<![\w.]):(?P<colon>\d+)"
    rf"|、(?P<comma>\d+)(?![-\d.])"
)
# 正文里的行号区间尾巴：`path:40-56` 与 `path:40..56` 两种写法都在用
PROSE_RANGE_RE = re.compile(r"^(?:-(?P<dash>\d+)|\.\.(?P<dots>\d+))")
PROSE_TEXT_FIELDS = ("label", "sublabel", "tag", "note")
PROSE_ELEMENT_COLLECTIONS = VERTEX_COLLECTIONS + ("edges", "lanes")
# 台账正文里也手写「路径:行号」，同一条底线要同判：SECURITY 全篇（安全面承诺的行号要活到兑现）
# 与 CHANGELOG 的最新发布段（发布说明里的引用是对该版本交付物的主张）。旧发布段不回判，原因见文件头
LEDGER_PROSE_FILES = (".github/SECURITY.md", "CHANGELOG.md")
# 提取数下限：字段改名、正则失配、图件被换掉都会让提取数塌下来而门照绿，按「门没跑成」退 2。
# 地板取历史档（refs 237 / prose 115）往下留一截：正常增删碰不到，结构性变化一定触发
MINIMUM_TOTAL_REFS = 200
MINIMUM_TOTAL_PROSE_REFS = 100


def load_diagrams():
    """读回所有候选图 JSON。返回 (路径, 数据) 列表与一条错误说明——解不开就是工件坏了。"""
    try:
        # 候选件的形状是 `<slug>.json`，一个点；archify 重出时旁挂的收据是 `<slug>.<步骤>.json`
        # （delivery / browser-check / finalize / finalize-summary），那些不是图，读成图会得到
        # 「这张图一条 sources 都没有」这种凭空造出来的红。按点的个数筛，将来多一种收据也不用回来改
        names = sorted(n for n in os.listdir(DIAGRAM_DIR) if n.endswith(".json") and n.count(".") == 1)
    except OSError as error:
        # 目录整个不见（被移动、改名，或从别的仓库树里跑这个脚本）也要退 2：这不是「没有违例」，
        # 而是判据压根没跑。抛 Python 栈会被 CI 读成退出码 1，看着像「查出了问题」
        return None, "读不到 %s：%s" % (DIAGRAM_DIR, error)
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
            if not (isinstance(element, dict) and "id" in element and "sources" not in element):
                continue
            # type: external 的参与者说的是「仓库之外的对端」（浏览器、curl、真实 CA），
            # 它按定义没有仓内证据可引，算进警告只会让这条清单永远清不空
            if str(element.get("type", "")) == "external":
                continue
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


def tracked_file_index():
    """文件名 → 仓库内路径列表，用来把正文里只写文件名的引用对上真文件。git 不可用时回 None。"""
    try:
        result = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    index = {}
    for line in result.stdout.splitlines():
        rel = line.strip().replace("\\", "/")
        if rel:
            index.setdefault(os.path.basename(rel), []).append(rel)
    return index


def prose_ref_path(match, owner, index):
    """把一条正文引用对到仓库内路径。写全路径直接认；只写文件名时按仓库内唯一同名文件补全，
    重名就不猜（猜错会把「引用有效」报给一个根本没被核对的行）。"""
    if match.group("path"):
        rel = match.group("path")
        if os.path.isfile(os.path.join(REPO, rel)):
            return rel
        if index is None:
            return None
        hits = index.get(os.path.basename(rel), [])
        return hits[0] if len(hits) == 1 else None
    if match.group("ext"):
        if owner is None or index is None:
            return None
        want = os.path.basename(owner).rsplit(".", 1)[0] + match.group("ext")
        for candidate in index.get(want, []):
            if os.path.dirname(candidate) == os.path.dirname(owner):
                return candidate
        return None
    return owner


def prose_ref_line(match):
    for key in ("full", "suffixed", "colon", "comma"):
        if match.group(key):
            return int(match.group(key))
    return None


def iter_prose_texts(data, name):
    """卡片正文与节点/边的文字——这些地方也手写着行号，与 sources 同一条底线要同判。"""
    for ci, card in enumerate(data.get("cards") or []):
        for ii, item in enumerate(card.get("items") or []):
            if isinstance(item, str):
                yield "%s/cards[%d].items[%d]" % (name, ci, ii), item
    for collection in PROSE_ELEMENT_COLLECTIONS:
        for i, element in enumerate(data.get(collection) or []):
            if not isinstance(element, dict):
                continue
            for field in PROSE_TEXT_FIELDS:
                value = element.get(field)
                if isinstance(value, str):
                    yield "%s/%s[%d].%s" % (name, collection, i, field), value
    for collection in VERTEX_COLLECTIONS:
        for i, element in enumerate(data.get(collection) or []):
            if not isinstance(element, dict):
                continue
            for si, source in enumerate(element.get("sources") or []):
                if isinstance(source, dict) and isinstance(source.get("label"), str):
                    yield "%s/%s[%d]/sources[%d].label" % (name, collection, i, si), source["label"]


def git_changed_files(revision):
    """自 revision 起被改过的文件集合。拿不到就返回 None，让新鲜度这段明确报「没跑成」。"""
    try:
        result = subprocess.run(
            ["git", "diff", "--name-only", "%s..HEAD" % revision, "--", "src", "tests", "samples", "CMakeLists.txt", "cmake",
             "scripts", ".github", "README.md", "CONTRIBUTING.md", "packaging", "benchmarks", "CMakePresets.json"],
            cwd=REPO, capture_output=True, text=True, check=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        sys.stderr.write("新鲜度判定跳过：git 不可用或修订号取不到（%s）\n" % error)
        return None
    return {line.strip().replace("\\", "/") for line in result.stdout.splitlines() if line.strip()}


def scan_prose_text(trail, text, index, line_cache, cited_by_path, allow_external_bare_names=False):
    """对一段正文跑「路径:行号」判据。

    @param trail 这段正文在图或台账里的位置（进消息用）
    @param allow_external_bare_names 只写裸文件名（不含 '/'）且对不到树内文件时，按「可能是仓库外的
           依赖头」记 WARN 跳过（SECURITY 里引 zlib 的 zconf.h 就属这类）；写全仓相对路径而读不到
           一律 FAIL。图那边的既有口径是两者都 FAIL，行为不变
    @return (已核对的引用条数, 新增 FAIL 列表, 新增 WARN 列表)
    """
    failures = []
    warnings = []
    matchedCount = 0
    owner = None
    for match in PROSE_REF_RE.finditer(text):
        line = prose_ref_line(match)
        rel = prose_ref_path(match, owner, index)
        if match.group("path") and rel:
            owner = rel
        if rel is None or line is None:
            if match.group("path"):
                if allow_external_bare_names and "/" not in match.group("path"):
                    warnings.append("REF_OUTSIDE_TREE %s -> %s（裸文件名且树内无同名文件：按仓库外引用跳过）" % (trail, match.group("path")))
                else:
                    failures.append("PROSE_MISSING_PATH %s -> %s（正文点名的文件不在树里）" % (trail, match.group("path")))
            # 只写了 `:123` 或 `、123` 而前面没有完整路径：续写没有可继承的对象，这条压根
            # 无法定位。判红等于逼人重写文案，所以略过——但它不等于「已核对过」
            continue
        matchedCount += 1
        if rel not in line_cache:
            absolute = os.path.join(REPO, rel)
            with open(absolute, encoding="utf-8", errors="replace") as handle:
                line_cache[rel] = handle.read().splitlines()
        lines = line_cache[rel]
        cited_by_path.setdefault(rel, set()).add(trail)
        cited = "%s:%d" % (rel, line)
        verdict, _ = classify_line(lines, line)
        if verdict == "BEYOND_EOF":
            failures.append("PROSE_LINE_BEYOND_EOF %s -> %s（文件只有 %d 行）" % (trail, cited, len(lines)))
        elif verdict == "BLANK":
            failures.append("PROSE_LINE_BLANK %s -> %s（引到空行，等于没有证据）" % (trail, cited))
        elif verdict == "COMMENT":
            warnings.append("PROSE_LINE_ON_COMMENT %s -> %s" % (trail, cited))
        range_match = PROSE_RANGE_RE.match(text[match.end():])
        if range_match is None:
            continue
        end_line = int(range_match.group("dash") or range_match.group("dots"))
        end_verdict, _ = classify_line(lines, end_line)
        if end_verdict == "BEYOND_EOF":
            failures.append("PROSE_END_LINE_BEYOND_EOF %s -> %s..%d" % (trail, cited, end_line))
        elif end_line < line:
            failures.append("PROSE_END_LINE_BEFORE_START %s -> %s..%d" % (trail, cited, end_line))
    return matchedCount, failures, warnings


def iter_ledger_prose_texts():
    """台账正文里的手写引用：SECURITY 全篇 + CHANGELOG 最新发布段。产出 (位置, 文本)。"""
    for relative in LEDGER_PROSE_FILES:
        absolute = os.path.join(REPO, relative)
        if not os.path.isfile(absolute):
            continue  # 缺文件：从「一份都没扫到」的地板里看得见，不必在这里另发错
        with open(absolute, encoding="utf-8", errors="replace") as handle:
            lines = handle.read().splitlines()
        insideLatestRelease = relative != "CHANGELOG.md"
        for number, text in enumerate(lines, 1):
            if relative == "CHANGELOG.md":
                if text.startswith("## ["):
                    if insideLatestRelease:
                        break  # 最新发布段到此结束
                    insideLatestRelease = True
                if not insideLatestRelease:
                    continue
            yield "%s:%d" % (relative, number), text


def iter_json_element_sources(data):
    """json 里带 id 与 sources 的元素 → {id: [(path, line)]}（交付 HTML 的证据块按这个分组）。"""
    out = {}

    def walk(node):
        if isinstance(node, dict):
            if "id" in node and isinstance(node.get("sources"), list):
                out[str(node["id"])] = [(str(source.get("path")), source.get("line")) for source in node["sources"]]
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(data)
    return out


HTML_EVIDENCE_RE = re.compile(r'<script id="archify-source-evidence-data" type="application/json">(.*?)</script>', re.S)


def check_delivery_html(name, data, failures, warnings):
    """交付 HTML 的内嵌证据块要与源 JSON 同解（见文件头那一条）。HTML 缺失只记警告。"""
    html_path = os.path.join(DIAGRAM_DIR, name[:-5] + ".html")
    if not os.path.isfile(html_path):
        warnings.append("DELIVERY_HTML_MISSING %s（没有同名 .html 交付件）" % name)
        return
    with open(html_path, encoding="utf-8", errors="replace") as handle:
        html = handle.read()
    match = HTML_EVIDENCE_RE.search(html)
    if match is None:
        failures.append("DELIVERY_HTML_NO_EVIDENCE %s（HTML 里没有 source-evidence 块：交付件不是这套工具链出的？）" % name)
        return
    try:
        blob = json.loads(match.group(1))
    except ValueError as error:
        failures.append("DELIVERY_HTML_BAD_EVIDENCE %s（证据块解不开：%s）" % (name, error))
        return

    pinned = str((data.get("meta") or {}).get("repository", {}).get("revision", "")).strip()
    htmlRevision = str((blob.get("repository") or {}).get("revision", ""))
    if htmlRevision != pinned:
        failures.append("DELIVERY_HTML_STALE_REVISION %s（HTML 钉 %s，JSON 钉 %s：交付件停在旧修订）"
                        % (name, htmlRevision[:8], pinned[:8]))

    jsonSources = iter_json_element_sources(data)
    htmlNodes = {str(key): [(str(item.get("path")), item.get("line")) for item in value] for key, value in (blob.get("nodes") or {}).items()}
    jsonTotal = sum(len(value) for value in jsonSources.values())
    if blob.get("referenceCount") != jsonTotal:
        failures.append("DELIVERY_HTML_STALE_COUNT %s（HTML 记 %s 条引用，JSON 现有 %d 条）" % (name, blob.get("referenceCount"), jsonTotal))
    for elementId in sorted(set(jsonSources) | set(htmlNodes)):
        if sorted(jsonSources.get(elementId, [])) != sorted(htmlNodes.get(elementId, [])):
            failures.append("DELIVERY_HTML_STALE_LINES %s/%s（HTML 与 JSON 的引用行位不同解）" % (name, elementId))


def main() -> int:
    parser = argparse.ArgumentParser(description="架构图 sources 引用的可追溯性检查")
    parser.add_argument("--stale-since", help="新鲜度基准修订号，默认取每张图 meta.repository.revision")
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
    total_prose = 0
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

        check_delivery_html(name, data, failures, warnings)

    index = tracked_file_index()
    for name, data in diagrams:
        for trail, text in iter_prose_texts(data, name):
            matchedCount, newFailures, newWarnings = scan_prose_text(trail, text, index, line_cache, cited_by_path)
            total_prose += matchedCount
            failures.extend(newFailures)
            warnings.extend(newWarnings)

    # 台账正文那一半（见 LEDGER_PROSE_FILES 与文件头）：裸文件名对不到树内文件只记警告（可能是
    # 仓库外的依赖头），全仓相对路径读不到仍是死引用
    for trail, text in iter_ledger_prose_texts():
        matchedCount, newFailures, newWarnings = scan_prose_text(trail, text, index, line_cache, cited_by_path, allow_external_bare_names=True)
        total_prose += matchedCount
        failures.extend(newFailures)
        warnings.extend(newWarnings)

    # 空集防护（见文件头）：提取数塌到地板以下、或有图一条 sources 都没有，都是「门没跑成」
    if total_refs < MINIMUM_TOTAL_REFS or total_prose < MINIMUM_TOTAL_PROSE_REFS:
        sys.stderr.write("提取数低于地板（refs=%d/%d，prose=%d/%d）：判据没吃到东西，按「门没跑成」退 2\n"
                         % (total_refs, MINIMUM_TOTAL_REFS, total_prose, MINIMUM_TOTAL_PROSE_REFS))
        return 2
    emptyDiagrams = sorted(name for name, count in per_diagram_refs.items() if count == 0)
    if emptyDiagrams:
        sys.stderr.write("这些图一条 sources 都没有（图件被换掉或结构改名了）：%s\n" % "、".join(emptyDiagrams))
        return 2

    for name in sorted(per_diagram_refs):
        print("DIAGRAM %s refs=%d" % (name, per_diagram_refs[name]))

    # 新鲜度这一半必须要么给出清单、要么当场说「没跑成」。此前的形状是「各图钉的修订不止一个就整段跳过并退 0」，
    # 于是九张图钉着两个修订的那几轮里，反向索引与 stale 清单一次都没产出，而作业是绿的——
    # 那份跳过把「门没跑」读成了「门通过」。基准取不到按文件头的承诺退 2（宁可让作业变红）
    bases = {}
    if args.stale_since:
        bases[args.stale_since] = sorted(name for name, _ in diagrams)
    else:
        without_revision = []
        for name, data in diagrams:
            pinned = str((data.get("meta") or {}).get("repository", {}).get("revision", "")).strip()
            if pinned:
                bases.setdefault(pinned, []).append(name)
            else:
                without_revision.append(name)
        if without_revision:
            sys.stderr.write("新鲜度判定没有基准：这些图没钉修订号 -> %s\n" % "、".join(sorted(without_revision)))
            return 2

    stale_union = set()
    for base in sorted(bases):
        changed = git_changed_files(base)
        if changed is None:
            sys.stderr.write("新鲜度判定没跑成：基准 %s 取不到变更清单（浅克隆里没有这个修订就是这种红，"
                             "跑这道门的作业要 fetch-depth: 0）\n" % base[:8])
            return 2
        stale_here = {path for path in cited_by_path if path in changed}
        print("STALE_BASE=%s diagrams_pinned_here=%d changed_cited_files=%d" % (base[:8], len(bases[base]), len(stale_here)))
        stale_union |= stale_here

    if stale_union:
        affected = sorted({trail for path in stale_union for trail in cited_by_path[path]})
        print("STALE_TOTAL changed_cited_files=%d affected_refs=%d" % (len(stale_union), len(affected)))
        for path in sorted(stale_union)[:15]:
            print("STALE_FILE %s（被 %d 处引用）" % (path, len(cited_by_path[path])))
        if len(stale_union) > 15:
            print("STALE_FILE …另有 %d 个被引文件自各基准后变更" % (len(stale_union) - 15))
        print("回图时按上面的引用位置逐条复核；这是清单不是判决")

    for line in failures:
        print("VIOLATION " + line)
    for line in warnings:
        print("WARN " + line)

    print("TOTAL_REFS=%d TOTAL_PROSE_REFS=%d FAIL=%d WARN=%d" % (total_refs, total_prose, len(failures), len(warnings)))
    if failures:
        print("ARCH_TRACE_GATE=FAIL")
        return 1
    print("ARCH_TRACE_GATE=OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
