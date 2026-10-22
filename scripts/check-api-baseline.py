#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""公开接口面基线：安装出去的每份公开头里，每条声明都要在基线登记过；改动只准显式重录。

覆盖面＝install(DIRECTORY src/ … PATTERN "*.h") 装走的头，排除名单见 cmake/AsynGyanisInstall.cmake；
规则形状读不出退 2。基线是排序纯文本，一行头文件相对路径与规范化声明，以制表符分隔，git diff 可读。
声明体不进基线；类体、命名空间体要进；预处理指令不参与（不同 DATABASE_HAS_* 面不同）。
diff 有删除即破坏性：需重录基线并抬高 CMAKE 版本 MAJOR，否则拒绝。本门只管源码声明面，
管不住布局与大小变化（私有成员类型、虚表顺序），别当成已覆盖。
用法：python3 scripts/check-api-baseline.py [--write]；退出码 0 同解、1 有差异、2 读不到必要信息。
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
INSTALL_FILE = REPOSITORY_ROOT / "cmake" / "AsynGyanisInstall.cmake"
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

    installText = INSTALL_FILE.read_text(encoding="utf-8")
    blockMatch = INSTALL_BLOCK_PATTERN.search(installText)
    if blockMatch is None:
        raise RuntimeError("cmake/AsynGyanisInstall.cmake 里找不到 "
                           "install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/ …) 那一段："
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
    """把源码切成声明段：一段到 depth 0 的 ';' 或 '{' 收口。

    类是公开面，类头记下并留在里面继续读成员；函数体或初始化列表只记签名后整组跳过。
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

    起手判定先剥前导属性标记（[[nodiscard]] 等），否则本仓公开方法会被整类漏掉；剥掉的只是
    判定探针，条目仍带原文——属性也是公开契约的一部分。
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
            "# 排除名单来自 cmake/AsynGyanisInstall.cmake 里 install(DIRECTORY src/ …) 的 EXCLUDE 项，现读不抄。",
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
