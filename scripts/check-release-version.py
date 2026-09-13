#!/usr/bin/env python3
"""发布版本一致性校验：CMake 版本号 / 更新日志最新发布段 / 最新标签三者必须一致。

三个取值来源各自独立，任何一处漂移（改了版本没写日志、打了标签没改版本）都会在这里露出来，
因此把它挂进 CI 就能挡住「标签与版本号对不上」这类只有发布时才发现的错。

用法：python scripts/check-release-version.py
退出码：0 一致；1 不一致；2 读不到必要信息（缺文件、缺提交历史）

尚未打过任何标签时只跳过标签这一项比较并给出提示——首个版本发布之前标签本来就不存在。
"""

import re
import subprocess
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
CMAKE_FILE = REPOSITORY_ROOT / "CMakeLists.txt"
CHANGELOG_FILE = REPOSITORY_ROOT / "CHANGELOG.md"

# project(AsynGyanis VERSION 1.0.0 ...)：只认 project() 里的这一处 VERSION
CMAKE_VERSION_PATTERN = re.compile(r"project\(\s*AsynGyanis\s+VERSION\s+(\d+\.\d+\.\d+)")
# ## [1.0.0] - 2026-09-13：发布段；[Unreleased] 不是版本，跳过
CHANGELOG_RELEASE_PATTERN = re.compile(r"^##\s*\[(\d+\.\d+\.\d+)\]")


def readCmakeVersion():
    text = CMAKE_FILE.read_text(encoding="utf-8")
    match = CMAKE_VERSION_PATTERN.search(text)
    return match.group(1) if match else None


def readLatestChangelogVersion():
    for line in CHANGELOG_FILE.read_text(encoding="utf-8").splitlines():
        match = CHANGELOG_RELEASE_PATTERN.match(line.strip())
        if match:
            return match.group(1)
    return None


def readLatestTag():
    """取最新标签并剥掉前缀 v；没有任何标签时返回空串。"""
    result = subprocess.run(
        ["git", "-C", str(REPOSITORY_ROOT), "tag", "--list", "--sort=-v:refname"],
        capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError("git tag 执行失败：" + result.stderr.strip())

    for tagName in result.stdout.split():
        if re.fullmatch(r"v\d+\.\d+\.\d+", tagName):
            return tagName[1:]
    return ""


def main():
    for required in (CMAKE_FILE, CHANGELOG_FILE):
        if not required.is_file():
            print(f"缺少必要文件：{required}")
            return 2

    cmakeVersion = readCmakeVersion()
    if cmakeVersion is None:
        print(f"{CMAKE_FILE.name} 里找不到 project(AsynGyanis VERSION x.y.z)")
        return 2

    changelogVersion = readLatestChangelogVersion()
    if changelogVersion is None:
        print(f"{CHANGELOG_FILE.name} 里找不到形如 '## [x.y.z] - 日期' 的发布段")
        return 2

    try:
        tagVersion = readLatestTag()
    except RuntimeError as error:
        print(error)
        return 2

    print(f"CMake 版本号      ：{cmakeVersion}")
    print(f"更新日志最新发布段：{changelogVersion}")
    print(f"最新标签          ：{tagVersion if tagVersion else '（尚无）'}")

    mismatches = []
    if changelogVersion != cmakeVersion:
        mismatches.append(f"更新日志最新发布段 {changelogVersion} 与 CMake 版本号 {cmakeVersion} 不一致")
    if tagVersion and tagVersion != cmakeVersion:
        mismatches.append(f"最新标签 v{tagVersion} 与 CMake 版本号 {cmakeVersion} 不一致")

    if mismatches:
        for mismatch in mismatches:
            print("不一致：" + mismatch)
        return 1
    if not tagVersion:
        print("通过：尚无标签，本次只比对了 CMake 版本号与更新日志")
    else:
        print("通过：三处版本号一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
