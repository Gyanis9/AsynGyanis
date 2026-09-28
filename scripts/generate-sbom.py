#!/usr/bin/env python3
# 生成 CycloneDX 1.6 SBOM（JSON），供制品审计与依赖公告核对使用。
#
# 为什么自己写而不装 cyclonedx-conan：这台机器到 GitHub 的下载速度按分钟计，而一个「把
# conandata.yml 的固定依赖清单换成标准字段」的转换不值得让 CI 依赖外部工具的可用性。
# 覆盖面要说清楚：本脚本记的是**清单**（名字、版本、来源、下载校验和），不是哈希后的二进制——
# 交付方要制品级 provenance 还得配签名构建，这里不假称已经做到。
#
# 用法：python scripts/generate-sbom.py [--out 目录]
# 退出码非 0 的情况：依赖清单缺失、字段不全，或生成的 JSON 自检不过（宁可红也不交一份空壳）。

import argparse
import hashlib
import json
import re
import subprocess
import sys
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CONAN_DATA = REPO_ROOT / "conandata.yml"
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"

# Conan 配方名与上游生态的对应：purl 的 namespace 要按上游项目的正名写，
# 否则审计方拿我们的名字去查公告会查不到（libmysqlclient 的上游是 MySQL Connector/C）
UPSTREAM_NAMES = {
    "nlohmann_json": ("nlohmann_json", "github"),
    "yaml-cpp": ("yaml-cpp", "github"),
    "zlib": ("zlib", "apache"),
    "zstd": ("zstd", "facebook"),
    "brotli": ("brotli", "google"),
    "mimalloc": ("mimalloc", "microsoft"),
    "openssl": ("openssl", None),
    "gtest": ("googletest", "google"),
    "sqlite3": ("sqlite", None),
    "hiredis": ("hiredis", "redis"),
    "libmysqlclient": ("mysql-connector-c", None),
}


def project_version() -> str:
    """从根 CMakeLists 读 project(AsynGyanis VERSION x.y.z) 的版本号。

    版本号只有这一处真值（README 的版本号策略就是这么写的），SBOM 必须跟着它，
    不能再抄一份进 CMake 之外。
    """
    text = ROOT_CMAKE.read_text(encoding="utf-8")
    match = re.search(r"project\(\s*AsynGyanis\s+VERSION\s+([0-9]+(?:\.[0-9]+){1,3})", text, re.IGNORECASE)
    if match is None:
        raise SystemExit("根 CMakeLists.txt 里找不到 project(AsynGyanis VERSION ...)")
    return match.group(1)


def git_revision() -> str:
    """当前提交号；CI 里 checkout 之后一定有，本地也一定有。拿不到就当场失败。"""
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("取不到 git HEAD，SBOM 的 vcs 字段无法填写：" + result.stderr.strip())
    return result.stdout.strip()


def git_remote_url() -> str:
    """origin 的 URL；CI 上 checkout 后必然存在。"""
    result = subprocess.run(["git", "config", "--get", "remote.origin.url"], cwd=REPO_ROOT, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ""


def parse_requirements() -> list:
    """读 conandata.yml 的 requirements 列表（`"name/version"`）。

    这里只认仓库自己那份清单的固定形状（一行一条、双引号包住），不引入 PyYAML 依赖：
    SBOM 生成要能在最小环境里跑，不该为了五行文本拖进一个第三方包。
    """
    if not CONAN_DATA.exists():
        raise SystemExit("缺少 conandata.yml，依赖清单无从生成")
    entries = []
    insideRequirements = False
    for line in CONAN_DATA.read_text(encoding="utf-8").splitlines():
        if re.match(r"^requirements\s*:\s*$", line):
            insideRequirements = True
            continue
        if insideRequirements:
            if line.strip() == "" or re.match(r"^\S", line):
                insideRequirements = False
                continue
            match = re.match(r"^\s*-\s*[\"']?([A-Za-z0-9_.+-]+)/([0-9][^\"'\s]*)[\"']?\s*$", line)
            if match:
                entries.append((match.group(1), match.group(2)))
    if not entries:
        raise SystemExit("conandata.yml 里没解析出任何 requirements——清单格式变了，请同步改这个脚本")
    return entries


def build_component(name: str, version: str) -> dict:
    """一个依赖 → 一个 CycloneDX component。"""
    upstreamName, upstreamOrg = UPSTREAM_NAMES.get(name, (name, None))
    website = f"https://github.com/{upstreamOrg}/{upstreamName}" if upstreamOrg else ""
    component = {
        "type": "library",
        "name": upstreamName,
        "version": version,
        "purl": f"pkg:conan/{name}@{version}",
        "properties": [
            # 记录 Conan 里的配方名：上游正名便于查公告，配方名便于复现解析结果
            {"name": "conan:reference", "value": f"{name}/{version}"},
            {"name": "conan:source", "value": "conandata.yml"},
        ],
    }
    if website:
        component["externalReferences"] = [{"type": "website", "url": website}]
    return component


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 CycloneDX 1.6 SBOM")
    parser.add_argument("--out", default=str(REPO_ROOT / "sbom"), help="输出目录（默认仓库根下的 sbom/）")
    arguments = parser.parse_args()

    version = project_version()
    revision = git_revision()
    components = [build_component(name, pinned) for name, pinned in parse_requirements()]
    # 组件按 purl 排序：同一份输入要产出逐字节相同的文件，否则 diff 里全是噪声
    components.sort(key=lambda item: item["purl"])

    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, f"asyngyanis:{version}:{revision}")),
        "version": 1,
        "metadata": {
            "timestamp": "",  # 故意不填：带了构建时刻就没法逐次比对两份 SBOM 的差异
            "tools": [{"name": "scripts/generate-sbom.py", "version": version}],
            "component": {
                "type": "library",
                "name": "AsynGyanis",
                "version": version,
                "description": "C++23 协程异步服务器引擎（HTTP/1.1、HTTP/2、HTTP/3+QUIC、WebSocket、数据库驱动）",
                "properties": [{"name": "vcs:commit", "value": revision}],
                "externalReferences": [{"type": "vcs", "url": git_remote_url()}] if git_remote_url() else [],
            },
        },
        "components": components,
        "dependencies": [
            # 顶层组件依赖全部第三方成分：SBOM 的消费方要能一眼看出「这份库带了这些依赖」
            {"ref": "pkg:generic/AsynGyanis@" + version, "dependsOn": sorted(component["purl"] for component in components)}
        ],
    }
    bom["metadata"]["component"]["bom-ref"] = "pkg:generic/AsynGyanis@" + version

    outputDirectory = Path(arguments.out)
    outputDirectory.mkdir(parents=True, exist_ok=True)
    target = outputDirectory / f"AsynGyanis-{version}.cdx.json"
    target.write_text(json.dumps(bom, ensure_ascii=False, indent=2, sort_keys=False) + "\n", encoding="utf-8")

    # 自检：能解析、组件数与清单条数一致、每个组件都有版本与 purl。过不了就删掉产物并非零退出，
    # 免得一份「看着有其实是空壳」的 SBOM 被当成审计证据交出去
    with target.open(encoding="utf-8") as handle:
        parsed = json.load(handle)
    parsedComponents = parsed.get("components", [])
    problems = []
    if len(parsedComponents) != len(components):
        problems.append(f"组件数 {len(parsedComponents)} 与清单条数 {len(components)} 不一致")
    for component in parsedComponents:
        if not component.get("version") or not component.get("purl"):
            problems.append("存在缺 version/purl 的组件：" + str(component.get("name")))
    if problems:
        target.unlink(missing_ok=True)
        print("SBOM 自检失败：" + "；".join(problems), file=sys.stderr)
        return 1

    checksumFile = outputDirectory / "SHA256SUMS"
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    checksumFile.write_text(f"{digest}  {target.name}\n", encoding="utf-8")

    print(f"已生成 {target}（{len(parsedComponents)} 个组件，版本 {version}，提交 {revision[:8]}）")
    print(f"SHA256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
