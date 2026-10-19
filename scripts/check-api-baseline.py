#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""公开接口面基线：安装出去的每一份公开头里，每条声明都要在基线登记过；改动只准显式重录。

为什么要这道门：docs/入门与构建.md 承诺「生产方与消费方必须保持编译器、CRT、标准库及 ABI 相关
配置一致」，而在那之前更要紧的问题是「发出去之后才发现这次把 API 改破了」。这道门把公开面
钉成一份可比对的文本：任何一次公开声明的增删改都会让 diff 非空，红着，直到有人显式重录基线。

判据的形状：
  * 覆盖面 = `install(DIRECTORY src/ … PATTERN "*.h" …)` 实际会装走的那一份头，
    排除名单从 CMakeLists 那段规则里现读（不在脚本里抄第二份名单，否则安装规则一改基线就悄悄失真）；
    那段规则的形状读不出来就退 2——「门没跑」绝不能读成「门通过」。
  * 基线是排序后的纯文本，一行一条 `<头文件相对路径>\t<规范化声明>`：不写时间戳、不写绝对路径，
    行序稳定，因此 `git diff` 直接可读。
  * 声明体（函数体、初始化列表的内容）不进基线：改实现不该动接口面。类体、命名空间体要进去，
    因为成员声明本身就是面。
  * 预处理指令不参与（同一份源码在不同 DATABASE_HAS_* 下交出的面不同，基线记的是源码那一层）。

与 SemVer 的联动：diff 里有「删除」就是破坏性——破坏要在两件事上都留痕：基线重录，以及
CMake 版本号的 MAJOR 比基线记的那一版更高。只改代码没抬 MAJOR 时这道门拒绝，并说清怎么办。

ABI 面为什么没做成这道门的一部分：Linux 侧交付的是静态库（没有 .so 可供 nm 取导出符号，
更没有两份 .so 可供 abidiff 对比），要补这一面就得为它单开一套共享形态构建预设，而共享形态
在 CI 里已经有一档（Windows 侧 BUILD_SHARED_LIBS），它的产物在 Windows 上、上一版标签的产物
不在——跨平台跨版本的 .so 对比没有可比的两侧。因此这道门管的是**源码声明面**：签名改动、
增删函数与类成员，那也正是 abidiff 报告里最先出现的那一类。它管不住的是布局与大小变化
（改私有成员的类型、改虚函数表顺序而不改声明文本），那一条如实记在这里，别把它当成已覆盖。

用法：
    python3 scripts/check-api-baseline.py            # 检查（CI 用）
    python3 scripts/check-api-baseline.py --write    # 重新生成基线
退出码：0 同解；1 有差异；2 读不到必要信息（CMake 规则形状变了、一个头都没扫到）。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Windows runner 的控制台默认按 cp1252 编码：差异清单里的中文一 print 就抛 UnicodeEncodeError，
# 读的人看不到「改破了哪一条声明」。与 check-ledger-dates.py 同形状，先钉两个流
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
CMAKE_FILE = REPOSITORY_ROOT / "CMakeLists.txt"
BASELINE_FILE = REPOSITORY_ROOT / "packaging" / "api-baseline.txt"

# install(DIRECTORY …/src/ … FILES_MATCHING PATTERN "*.h" PATTERN "<名字>" EXCLUDE …)
INSTALL_BLOCK_PATTERN = re.compile(
    r"install\(\s*DIRECTORY\s+\$\{CMAKE_SOURCE_DIR\}/src/.*?\)\n", re.DOTALL)
EXCLUDED_NAME_PATTERN = re.compile(r'PATTERN\s+"([^"]+\.h)"\s+EXCLUDE')
CMAKE_VERSION_PATTERN = re.compile(r"project\(\s*AsynGyanis\s+VERSION\s+(\d+)\.(\d+)\.(\d+)")

# 一条声明的起手：模板头、类型定义、访问标签与常见限定符开头的行。用来把控制流与残留碎片筛掉
DECLARATION_START_PATTERN = re.compile(
    r"^(template\s*<|export\s|module|namespace|class|struct|union|enum|using|typedef|constexpr|consteval|"
    r"constinit|extern|static|inline|virtual|explicit|friend|co_?routine|noexcept|alignas|"
    r"[\w:]+_API|[\w:]+<|[A-Za-z_~][\w:]*\s*[\w:*&<])")
# 类体 / 命名空间体：遇到这种_pending 段落时不跳过花括号组，因为里面的成员声明也属于公开面
SCOPE_OPEN_PATTERN = re.compile(
    r"^(?:(?:template\s*<[^>]*>\s*)?(?:namespace|(?:class|struct|union|enum)(?:\s+(?:class|constexpr))?)\b)")
ACCESS_LABEL_PATTERN = re.compile(r"\b(public|private|protected)\s*:")
# 前导属性标记（可连续多条）：判定声明起手时先剥掉它们，条目文本里保留原文
LEADING_ATTRIBUTE_PATTERN = re.compile(r"^(?:\s*\[\[.*?\]\])+")
LINE_COMMENT_PATTERN = re.compile(r"//[^\n]*")
BLOCK_COMMENT_PATTERN = re.compile(r"/\*.*?\*/", re.DOTALL)
PREPROCESSOR_PATTERN = re.compile(r"^[ \t]*#[^\n]*\n", re.MULTILINE)


def trackedPublicHeaders():
    """按安装规则现读公开头清单：tracked 的 src/**/*.h 减去 install 明确排除的那几个名字。"""

    cmakeText = CMAKE_FILE.read_text(encoding="utf-8")
    blockMatch = INSTALL_BLOCK_PATTERN.search(cmakeText)
    if blockMatch is None:
        raise RuntimeError("CMakeLists.txt 里找不到 install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/ …) 那一段："
                           "覆盖面读本段现读，规则改了要同步改本脚本，不能退回抄一份名单")

    excludedNames = set(EXCLUDED_NAME_PATTERN.findall(blockMatch.group(0)))
    listed = subprocess.run(["git", "-C", str(REPOSITORY_ROOT), "ls-files", "src"],
                            capture_output=True, text=True, check=True).stdout.split()
    headers = [item for item in listed if item.endswith(".h") and Path(item).name not in excludedNames]
    return sorted(headers), excludedNames


def stripNonCode(text):
    """剥掉注释与预处理指令：两者都不算接口面（注释改了不该惊动这道门）。"""

    text = BLOCK_COMMENT_PATTERN.sub(" ", text)
    text = LINE_COMMENT_PATTERN.sub(" ", text)
    return PREPROCESSOR_PATTERN.sub("\n", text)


def splitDeclarations(text):
    """把源码切成声明段：一段到 depth 0 的 ';' 收口，或在 depth 0 遇到 '{' 时收口。

    遇到 '{' 时分两种走法：pending 段是类 / 结构 / 枚举 / 命名空间的开头，就把这一段（类头）
    单独记下并**留在里面继续读**——成员声明同样是公开面；是函数体或初始化列表，就记下签名后
    整组跳过——改实现不该惊动接口门。
    """

    declarations = []
    pending = []
    index = 0
    total = len(text)
    parenthesisDepth = 0
    bracketDepth = 0

    def pendingText():
        return normalizeSpace("".join(pending))

    def emit():
        candidate = pendingText()
        pending.clear()
        if candidate and not candidate.startswith("#"):
            declarations.append(candidate)

    def skipBraceGroup(startIndex):
        """跳过一个配对的花括号组，返回闭合括号之后的位置。"""

        depth = 0
        cursor = startIndex
        while cursor < total:
            if text[cursor] == "{":
                depth += 1
            elif text[cursor] == "}":
                depth -= 1
                if depth == 0:
                    return cursor + 1
            cursor += 1
        return total

    while index < total:
        character = text[index]
        if character == ";" and parenthesisDepth == 0 and bracketDepth == 0:
            emit()
            index += 1
            continue
        if character == "{":
            isScopeBody = bool(SCOPE_OPEN_PATTERN.match(pendingText()))
            emit()
            if not isScopeBody:
                index = skipBraceGroup(index)
                continue
            index += 1
            continue
        if character == "}":
            # 作用域体闭合：里面的成员已各自成段，这里清掉尾巴并把紧随其后的 ';' 一起吞掉
            pending.clear()
            if index + 1 < total and text[index + 1] == ";":
                index += 1
            index += 1
            continue
        if character == "(":
            parenthesisDepth += 1
        elif character == ")":
            parenthesisDepth = max(0, parenthesisDepth - 1)
        elif character == "[":
            bracketDepth += 1
        elif character == "]":
            bracketDepth = max(0, bracketDepth - 1)
        pending.append(character)
        index += 1

    trailing = pendingText()
    if trailing:
        declarations.append(trailing)
    return declarations


def normalizeSpace(text):
    """空白压成单个空格并去掉访问标签：基线要比的是声明本身，不是它的排版。"""

    collapsed = " ".join(text.split())
    collapsed = ACCESS_LABEL_PATTERN.sub(" ", collapsed)
    collapsed = collapsed.replace("{ ", "{").replace(" }", "}")
    return collapsed.strip()


def filterDeclarations(declarations):
    """只留像声明的段：过短、控制流碎片、纯标点都筛掉。

    起手判定要先剥掉前导的属性标记（`[[nodiscard]]` 这一类）——本仓的公开方法几乎每一条都带它，
    按原样判定会把整个 `[[nodiscard]]` 面当成「不是声明」而静默漏掉，那道门就成了假门。
    剥掉的只是判定用的探针，条目本身仍带原文：属性也是公开契约的一部分。
    """

    kept = []
    for declaration in declarations:
        cleaned = declaration.strip(" ;")
        probe = LEADING_ATTRIBUTE_PATTERN.sub("", cleaned).lstrip()
        if len(probe) < 3 or not DECLARATION_START_PATTERN.match(probe):
            continue
        if probe.split(" ")[0] in {"return", "else", "case", "default", "do", "while", "for", "if", "switch"}:
            continue
        kept.append(cleaned)
    return kept


def buildBaselineEntries():
    headers, excludedNames = trackedPublicHeaders()
    entries = []
    for header in headers:
        text = stripNonCode((REPOSITORY_ROOT / header).read_text(encoding="utf-8"))
        for declaration in filterDeclarations(splitDeclarations(text)):
            entries.append(header + "\t" + declaration)
    return sorted(entries), len(headers), excludedNames


def readExistingBaseline():
    if not BASELINE_FILE.is_file():
        return None, None
    lines = BASELINE_FILE.read_text(encoding="utf-8").splitlines()
    version = None
    entries = []
    for line in lines:
        if line.startswith("# version="):
            version = line.split("=", 1)[1].strip()
        elif line and not line.startswith("#"):
            entries.append(line)
    return version, entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="把当前实测的公开面写成基线")
    arguments = parser.parse_args()

    versionMatch = CMAKE_VERSION_PATTERN.search(CMAKE_FILE.read_text(encoding="utf-8"))
    if versionMatch is None:
        print("CMakeLists.txt 里找不到 project(AsynGyanis VERSION x.y.z)")
        return 2

    try:
        entries, headerCount, excludedNames = buildBaselineEntries()
    except RuntimeError as error:
        print(str(error))
        return 2

    if headerCount == 0:
        print("一个公开头都没扫到：覆盖面判据本身坏了，按失败处理而不是「没有差异」")
        return 2

    majorVersion = int(versionMatch.group(1))

    if arguments.write:
        BASELINE_FILE.parent.mkdir(parents=True, exist_ok=True)
        header = [
            "# AsynGyanis 公开接口面基线：由 scripts/check-api-baseline.py --write 生成，请勿手改",
            "# 一行一条「头文件相对路径<TAB>规范化声明」，整体排序，不含时间戳与绝对路径。",
            "# 排除名单来自 CMakeLists.txt 里 install(DIRECTORY src/ …) 的 EXCLUDE 项，现读不抄。",
            "# version=%d.%d.%d" % (majorVersion, int(versionMatch.group(2)), int(versionMatch.group(3))),
            "# headers=%d entries=%d" % (headerCount, len(entries)),
        ]
        BASELINE_FILE.write_text("\n".join(header + entries) + "\n", encoding="utf-8", newline="\n")
        print("API_BASELINE_WRITE headers=%d entries=%d file=%s" % (headerCount, len(entries), BASELINE_FILE.relative_to(REPOSITORY_ROOT)))
        return 0

    previousVersion, previousEntries = readExistingBaseline()
    if previousEntries is None:
        print("没有基线文件（packaging/api-baseline.txt）：先跑 --write 生成第一份基线")
        return 2

    oldSet = set(previousEntries)
    newSet = set(entries)
    added = sorted(newSet - oldSet)
    removed = sorted(oldSet - newSet)

    print("API_BASELINE headers=%d entries=%d baseline_entries=%d" % (headerCount, len(entries), len(previousEntries)))
    print("API_BASELINE excluded_by_install_rule=%d" % len(excludedNames))

    if not added and not removed:
        if previousVersion and int(previousVersion.split(".")[0]) != majorVersion:
            print("API_BASELINE 版本号 MAJOR 与基线记录不一致但接口面一字未改：这是版本笔误，按失败处理")
            print("API_GATE=FAIL")
            return 1
        print("API_GATE=OK")
        return 0

    print("API_BASELINE added=%d removed=%d" % (len(added), len(removed)))
    for sample in added[:15]:
        print("  + " + sample)
    for sample in removed[:15]:
        print("  - " + sample)
    if len(added) > 15 or len(removed) > 15:
        print("  （只列前 15 条，完整的差异用 git diff 看基线文件本身）")

    if removed:
        baselineMajor = int(previousVersion.split(".")[0]) if previousVersion else None
        if baselineMajor is not None and majorVersion <= baselineMajor:
            print("API_BASELINE 判据：删除或改了签名的声明共 %d 条，这是破坏性改动，而版本号 MAJOR 还是 %s"
                  "（基线记的是 %s）——破坏要在两处留痕：抬 MAJOR，再跑 --write 重录基线"
                  % (len(removed), str(majorVersion), str(baselineMajor)))
        else:
            print("API_BASELINE 判据：有 %d 条声明被删除或改了签名（破坏性）。MAJOR 已抬升，"
                  "重录基线即可：python3 scripts/check-api-baseline.py --write" % len(removed))
    else:
        print("API_BASELINE 判据：只新增（%d 条），没有删除。重录基线：python3 scripts/check-api-baseline.py --write"
              % len(added))
    print("API_GATE=FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
