#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""复杂度基线冻禁：最长函数只准降不准升。

判据的形状：
  * 任何 .cpp 里「一个函数体的行数」超过 FLOOR_LINES 就要在基线里登记过（头文件刻意不量，理由见
    SOURCE_SUFFIXES 那段注释：类内内联体的花括号计数会被模板与深层协程带偏，量出谁都不信的数）；
  * 登记过的文件按各自记录的上限判，没登记过的文件一旦越过 FLOOR_LINES 即失败；
  * 低于记录值不报告警——那是改进，改完把基线一起降下来（--write 重新生成）；
  * 一个文件都没量到（src 被挪走、SCAN_DIRS 或后缀被改错）判红而不是判过——「门没跑」不能读成「门通过」；
  * 基线里指向已消失文件的条目单独点名（不判红）：它们是没人认领的天花板，随 --write 才会清掉。

为什么是一道脚本而不是 clang-tidy 的一条检查：仓库里那个 tidy 作业是「只报告不阻塞」档，
挂上去的东西不会让任何人流红；冻结判据必须有牙齿。

用法：
    python3 scripts/check-complexity-baseline.py            # 检查（CI 用）
    python3 scripts/check-complexity-baseline.py --write     # 重新生成基线
退出码：0 通过；1 违例；2 基线或输入本身有问题。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

FLOOR_LINES = 100
BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "complexity-baseline.json")
SCAN_DIRS = ("src",)
# 已知天花板：只量 .cpp。头文件里的内联体在类内，一旦体内出现「本层没闭合的花括号形状」（模板段、
# 嵌套很深的协程），括号计数器会把外层块一起算进来，量出 900 行这种谁都不信的数——修它要引一个真正
# 的 C++ 词法器，而这条判据的目的是冻住最长的几个函数。.cpp 那一侧的读数已与人工目测逐一对齐
# （393/371/363/342/330/286）。要覆盖头文件时改用 libclang 取 FunctionDecl 的起止行，别再调正则。
SOURCE_SUFFIXES = (".cpp",)

# 函数体的起点：定义行以「限定名 + 名字 + (」开头，参数表可以跨几行（中间不出现 ; 或花括号），
# 收口行的下一非空行必须是单独一个 '{'。两条判据都要，缺一就出错：
#   * 只看「有 ( 且开头像声明符」会把多行签名的**中间那行**（`const std::function<bool()> &isAlive,`）
#     当成函数起点，于是从签名中段数到外层块的末尾，一个 900 行的假函数就这么出来了——故要求
#     上一非空行是「一条语句/一个块/一个访问段的结尾」，不接受以 ',' 或 '(' 收尾的续行；
#   * 只要求同行收口又会漏掉头文件里那些签名换行的内联体（HttpSession.h 的几条正是这种形状）。
FUNCTION_HEAD = re.compile(r"^[A-Za-z_~][\w:<>,\s\*&]*(::[\w~<>]+)?\s*\(")
FUNCTION_TAIL = re.compile(r"\)\s*((const|noexcept|override|final)\s*)*(\{)?$")
CONTINUATION_ENDINGS = (',', '(', '&&', '||', '?', '.', '->', '+', '-', '*', '/', '=')
CONTROL_KEYWORDS = ("if", "for", "while", "switch", "return", "else", "do", "case")
MAX_SIGNATURE_LINES = 12


def previousStatementEnds(lines, index):
    """上一条非空行是不是一条语句/一个块/一个访问段的结尾（不是续行）。"""
    cursor = index - 1
    while cursor >= 0 and not lines[cursor].strip():
        cursor -= 1
    if cursor < 0:
        return True
    previous = lines[cursor].strip()
    if previous.startswith(('//', '/*', '*')) or previous == '*/':
        # 注释行不算续行，但它自己也可能是被注释掉的代码——退一步看注释再上面那一行
        return previousStatementEnds(lines, cursor)
    if previous.endswith(CONTINUATION_ENDINGS):
        return False
    return True


INLINE_EMPTY_BRACES = re.compile(r"\{[^{}]*\}")


def stripInlineBraces(body):
    """去掉参数表里的就地初始化/默认实参（`= {}`、`= Options{}`）——它们是值，不是块的开始。"""
    previous = None
    while previous != body:
        previous = body
        body = INLINE_EMPTY_BRACES.sub("", body)
    return body


def signatureEndIndex(lines, index):
    """从定义行起找参数表的收口行；参数表里出现 ; 或花括号就不算一条签名。

    括号深度要跨行累计：一条签名的后半截常常单独成行（`std::optional<X> last)`)，
    按「这一行左右括号相等」判会漏掉它，接着撞上函数体的 '{' 就整条判废。
    默认实参里的 `{}` 要先剥掉，否则那一行带着一个多出来的 '{'，整个体的闭合点会被推后一层
    ——推后的表现不是报错，而是把函数一路数到文件末尾，量出一个谁都不会信的假读数。
    """
    depth = 0
    for cursor in range(index, min(index + MAX_SIGNATURE_LINES, len(lines))):
        raw = lines[cursor]
        body = stripInlineBraces(raw)
        if ';' in body or '}' in body:
            return None
        depth += body.count('(') - body.count(')')
        if depth < 0:
            return None
        if depth == 0 and FUNCTION_TAIL.search(body.strip()):
            return cursor
        if '{' in body and depth > 0:
            return None
    return None


def codeOnly(line, state):
    """去掉字符串/字符字面量的内容与注释，只留下真正的代码字符。

    不剥这一步会数错花括号：日志格式串里的 `{}` 成对时没事，一条只写了一半的（JSON 样例、
    或者注释里引用的半个大括号）就会让深度永远回不到零，于是一个几百行的函数被读成 900 行。
    state 是一个集合，跨行携带块注释与 raw string 的状态。
    """
    out = []
    index = 0
    length = len(line)
    while index < length:
        rawDelim = next((key for key in state if key.startswith("raw-string:")), None)
        if rawDelim:
            terminator = ")" + rawDelim.split(":", 1)[1] + '"'
            end = line.find(terminator, index)
            if end < 0:
                return "".join(out), state
            state.discard(rawDelim)
            index = end + len(terminator)
            continue
        if "block-comment" in state:
            end = line.find("*/", index)
            if end < 0:
                return "".join(out), state
            state.discard("block-comment")
            index = end + 2
            continue
        char = line[index]
        following = line[index:index + 2]
        if following == "//":
            break
        if following == "/*":
            state.add("block-comment")
            index += 2
            continue
        if char == "R" and line[index + 1:index + 2] == '"':
            # raw string：定界符里可以放 JSON，字面量内的花括号一律不算代码
            opener = line.find("(", index + 2)
            if opener > 0:
                delimiter = line[index + 2:opener]
                state.add("raw-string:" + delimiter)
                index = opener + 1
                continue
        if char in ('"', "'"):
            quote = char
            index += 1
            while index < length:
                if line[index] == "\\":
                    index += 2
                    continue
                if line[index] == quote:
                    index += 1
                    break
                index += 1
            continue
        out.append(char)
        index += 1
    return "".join(out), state


def longest_body(lines):
    """返回文件里最长的一个函数体行数（整条签名 + 配对的花括号）。"""
    best = 0
    index = 0
    total = len(lines)
    state = set()
    cleaned = []
    for line in lines:
        strippedLine, state = codeOnly(line, state)
        cleaned.append(strippedLine)
    lines = cleaned
    while index < total:
        line = lines[index].strip()
        if not FUNCTION_HEAD.match(line) or line.split('(')[0].strip() in CONTROL_KEYWORDS:
            index += 1
            continue
        if not previousStatementEnds(lines, index):
            index += 1
            continue
        signatureEnd = signatureEndIndex(lines, index)
        if signatureEnd is None:
            index += 1
            continue
        openingLine = lines[signatureEnd]
        if openingLine.rstrip().endswith('{'):
            openIndex = signatureEnd
        else:
            openIndex = signatureEnd + 1
            while openIndex < total and not lines[openIndex].strip():
                openIndex += 1
            if openIndex >= total or lines[openIndex].strip() != '{':
                index += 1
                continue
        depth = 0
        scan = openIndex
        while scan < total:
            for char in lines[scan]:
                if char == '{':
                    depth += 1
                elif char == '}':
                    depth -= 1
            if depth <= 0:
                best = max(best, scan - index + 1)
                index = scan + 1
                break
            scan += 1
        else:
            break
    return best


def measure(root):
    """扫出每个源文件的最长函数体行数，只保留越过地板的那些。"""
    measured = {}
    for directory in SCAN_DIRS:
        base = os.path.join(root, directory)
        for dirpath, _dirnames, filenames in os.walk(base):
            for name in filenames:
                if not name.endswith(SOURCE_SUFFIXES):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                with open(full, encoding="utf-8", errors="replace") as handle:
                    lines = handle.read().splitlines()
                longest = longest_body(lines)
                if longest > FLOOR_LINES:
                    measured[rel] = longest
    return measured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="把当前实测写成基线")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    measured = measure(root)

    if args.write:
        with open(BASELINE, "w", encoding="utf-8") as handle:
            json.dump({"floor-lines": FLOOR_LINES,
                       "note": "每个文件登记的是本轮实测的最长函数体行数；只准降不准升",
                       "files": dict(sorted(measured.items()))},
                      handle, ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
        print("BASELINE_FILES=%d written" % len(measured))
        return 0

    if not os.path.exists(BASELINE):
        sys.stderr.write("找不到 %s；先跑 --write 生成基线\n" % BASELINE)
        return 2

    with open(BASELINE, encoding="utf-8") as handle:
        baseline = json.load(handle)["files"]

    violations = []
    if not measured:
        # 实测集为空就是这道门没跑到：src 被挪走、SCAN_DIRS 写错、后缀判据改掉都会走到这里。
        # 早先的形状是「只对实测集判」，于是量到 0 个文件也报 OK——把「没跑」读成「通过」
        sys.stderr.write("一个源文件都没量到（SCAN_DIRS=%s，后缀=%s）：这道门没有跑到东西，判红\n"
                         % ("/".join(SCAN_DIRS), "/".join(SOURCE_SUFFIXES)))
        print("COMPLEXITY_GATE=FAIL")
        return 2

    for path, longest in sorted(measured.items()):
        ceiling = baseline.get(path)
        if ceiling is None:
            violations.append("%s 最长函数 %d 行：新越线文件，基线里没有这一项" % (path, longest))
        elif longest > ceiling:
            violations.append("%s 最长函数 %d 行 > 基线 %d 行" % (path, longest, ceiling))

    # 基线里的死条目：文件已删或已降到地板以下，这一项就没人再判。它们不会让任何作业变红，
    # 于是「天花板」名单只会越来越长——点名出来，随下一次 --write 收敛
    dead_entries = sorted(path for path in baseline if path not in measured)

    print("MEASURED_FILES=%d FLOOR=%d DEAD_BASELINE_ENTRIES=%d" % (len(measured), FLOOR_LINES, len(dead_entries)))
    for path in dead_entries[:15]:
        print("WARN 基线里有这一项而实测集里没有（文件已删/改名或已降到地板下）：%s" % path)
    if len(dead_entries) > 15:
        print("WARN …另有 %d 项同样对不上" % (len(dead_entries) - 15))
    for offender in violations:
        print("VIOLATION " + offender)
    if violations:
        print("COMPLEXITY_GATE=FAIL")
        return 1
    print("COMPLEXITY_GATE=OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
