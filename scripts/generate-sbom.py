#!/usr/bin/env python3
"""生成 CycloneDX 1.6 SBOM（JSON），供制品审计与依赖公告核对使用。

自己写而不装 cyclonedx-conan：转换 conandata.yml 的固定清单不值得让 CI 依赖外部工具。
记的是清单（名字、版本、来源、下载校验和），不是哈希后的二进制；制品级 provenance 需签名构建。
用法：python scripts/generate-sbom.py [--out 目录]；退出码非 0：清单缺失、字段不全或 JSON 自检不过。
"""

import argparse
import ast
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
WATCH_LIST = REPO_ROOT / "packaging" / "dependency-watch.json"
LICENSE_FILE = REPO_ROOT / "LICENSE"
CONAN_RECIPE = REPO_ROOT / "packaging" / "conan" / "conanfile.py"


def load_watch_entries() -> dict:
    """读 packaging/dependency-watch.json 的 dependencies 段。

    每条依赖的静态元数据（上游正名、许可、官网）只住这儿：加依赖时清单与台账各补一条，
    SBOM 不维护第二份映射。许可缺失留给 main 的自检统一报。
    """
    if not WATCH_LIST.exists():
        raise SystemExit("缺少 packaging/dependency-watch.json，依赖元数据无从生成")
    document = json.loads(WATCH_LIST.read_text(encoding="utf-8"))
    entries = document.get("dependencies")
    if not isinstance(entries, dict) or not entries:
        raise SystemExit("台账里没有 dependencies 段，或它是空的")
    return entries


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


def read_truth_source(path: Path, purpose: str) -> str:
    """读一处判据的真值来源；文件不在或读不了都是判据没跑成，红在明面上而不是抛 OSError 栈。

    真值缺位走 SystemExit 的成文口径，与本脚本对台账、对 conandata.yml 的处理一致；
    权限与路径是目录同样走这个口径：判据没跑成不是脚本崩溃，接收方要看的是同一句说明。
    """
    displayPath = path.relative_to(REPO_ROOT).as_posix()
    if not path.exists():
        raise SystemExit(f"缺少 {displayPath}，{purpose}")
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise SystemExit(f"读不了 {displayPath}（{error.strerror or error}），{purpose}") from error


def project_license() -> str:
    """从 LICENSE 正文判项目自有许可，读不出就是说不清自己是什么许可，按红处理。"""
    text = read_truth_source(LICENSE_FILE, "自有许可无从判型，三处同解的核对跑不成")
    if "Apache License" in text and "Version 2.0" in text:
        return "Apache-2.0"
    if "Permission is hereby granted, free of charge" in text:
        return "MIT"
    raise SystemExit("LICENSE 判不出许可：只认 Apache-2.0 与 MIT 两种正文形状")


def recipe_license_assignments() -> list:
    """读配方 ConanFile 类体里对 license 的类级赋值，按出现次序返回字符串常量值。

    只认类体这一层的普通赋值与带注解赋值：Conan 从类属性读元数据，方法体里的同名赋值
    不参与；文本扫描分不清这一层，换行与转义还会让命中数偏离真实赋值数。
    """
    text = read_truth_source(CONAN_RECIPE, "配方许可读不到，三处同解的核对跑不成")
    try:
        tree = ast.parse(text, filename=CONAN_RECIPE.as_posix())
    except SyntaxError as error:
        raise SystemExit(
            f"packaging/conan/conanfile.py 第 {error.lineno} 行解析不了，配方许可无从核对"
        ) from error
    values = []
    for node in tree.body:
        if not isinstance(node, ast.ClassDef):
            continue
        # 只认直接继承 ConanFile 的配方类：同文件里别的辅助类不承载包元数据
        isRecipeClass = any(
            (isinstance(base, ast.Name) and base.id == "ConanFile")
            or (isinstance(base, ast.Attribute) and base.attr == "ConanFile")
            for base in node.bases
        )
        if not isRecipeClass:
            continue
        for statement in node.body:
            if isinstance(statement, ast.Assign):
                names = [target.id for target in statement.targets if isinstance(target, ast.Name)]
                value = statement.value
            elif isinstance(statement, ast.AnnAssign) and isinstance(statement.target, ast.Name):
                names = [statement.target.id]
                value = statement.value
            else:
                continue
            if "license" not in names:
                continue
            if isinstance(value, ast.Constant) and isinstance(value.value, str):
                values.append(value.value)
    return values


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
    """读 conandata.yml 的 requirements 列表（"name/version"）。

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


def build_component(name: str, version: str, entry: dict) -> dict:
    """一个依赖 → 一个 CycloneDX component（名字、官网与许可都取自台账条目）。"""
    # purl 与展示名按上游正名写（台账的 purlName），否则审计方拿我们的配方名去查公告会查不到
    upstreamName = entry.get("purlName") or name
    website = entry.get("homepage", "")
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
    # 许可：表达式（含 WITH 的）按 CycloneDX 的 expression 写，单一标识按 id 写
    licenseValue = entry.get("license", "")
    if licenseValue:
        component["licenses"] = [{"expression": licenseValue}] if " " in licenseValue else [{"license": {"id": licenseValue}}]
    return component


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 CycloneDX 1.6 SBOM")
    parser.add_argument("--out", default=str(REPO_ROOT / "sbom"), help="输出目录（默认仓库根下的 sbom/）")
    arguments = parser.parse_args()

    outputDirectory = Path(arguments.out)
    outputDirectory.mkdir(parents=True, exist_ok=True)
    # 输出目录一到手就先收掉上一轮的同名产物：真值随后才读，读不出来时目录里也不能留着
    # 上一轮的 SBOM 与 SHA256SUMS 冒充本轮结果。这一步在任何真值读取之前。
    for staleArtifact in outputDirectory.glob("AsynGyanis-*.cdx.json"):
        staleArtifact.unlink(missing_ok=True)
    checksumFile = outputDirectory / "SHA256SUMS"
    checksumFile.unlink(missing_ok=True)

    version = project_version()
    # 自有许可只判一次：写产物与自检读同一个值，不给「同一轮里读出两种答案」留口子
    licenseId = project_license()
    revision = git_revision()
    # origin URL 同样只读一次：两次读之间配置变了，同一份 SBOM 的两处 vcs 信息就会分叉
    remoteUrl = git_remote_url()
    watchEntries = load_watch_entries()
    requirements = parse_requirements()
    # 每条依赖都要在台账里有条目：名与许可从那里读，缺了就是「新增依赖没走登记」
    missingEntries = [name for name, _ in requirements if name not in watchEntries]
    if missingEntries:
        raise SystemExit("这些依赖没在 packaging/dependency-watch.json 里登记：" + "、".join(missingEntries))
    components = [build_component(name, pinned, watchEntries[name]) for name, pinned in requirements]
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
                "licenses": [{"license": {"id": licenseId}}],
                "properties": [{"name": "vcs:commit", "value": revision}],
                "externalReferences": [{"type": "vcs", "url": remoteUrl}] if remoteUrl else [],
            },
        },
        "components": components,
        "dependencies": [
            # 顶层组件依赖全部第三方成分：SBOM 的消费方要能一眼看出「这份库带了这些依赖」
            {"ref": "pkg:generic/AsynGyanis@" + version, "dependsOn": sorted(component["purl"] for component in components)}
        ],
    }
    bom["metadata"]["component"]["bom-ref"] = "pkg:generic/AsynGyanis@" + version

    target = outputDirectory / f"AsynGyanis-{version}.cdx.json"
    # 写入、自检与写校验和共用一处清理：任何出口（含 Ctrl-C）都先收掉两个文件再让异常
    # 照原样冒出去，盘上不留半份 SBOM 或与它配不上对的 SHA256SUMS
    try:
        # 行尾钉成 LF：跨平台逐字节稳定（Windows 文本模式默认写 CRLF，
        # POSIX 侧 sha256sum -c 会读不了）
        payload = json.dumps(bom, ensure_ascii=False, indent=2, sort_keys=False) + "\n"
        target.write_text(payload, encoding="utf-8", newline="\n")

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
            # 许可必须在：新增依赖而台账没补 license 字段时，这里当场红——第三方台账的「许可」一面
            # 不能靠人记着补（台账的 license 字段与 SBOM 的 licenses 是同一份数据的两个出口）
            if not component.get("licenses"):
                problems.append("存在缺 licenses 的组件（补 packaging/dependency-watch.json 的 license 字段）：" + str(component.get("name")))

        selfComponent = parsed.get("metadata", {}).get("component", {})
        # 自家许可也有两个出口（LICENSE 正文与包元数据），与依赖台账同一条底线：分叉就红
        selfLicenses = selfComponent.get("licenses")
        if not selfLicenses:
            problems.append("SBOM 的顶层组件缺 licenses：自家许可不能只在 LICENSE 里说一遍")
        else:
            # 缺键已经单独报过，不再补一条「'' 与 … 不同解」：一个根因只许占一条问题
            declared = (selfLicenses[0].get("license") or {}).get("id", "")
            if declared != licenseId:
                problems.append(f"顶层组件许可 {declared!r} 与 LICENSE 判型 {licenseId!r} 不同解")

        # 配方的 license 类级赋值只认唯一一处：Conan 多处赋值时取哪一处由它自己决定，
        # 判据无法认定哪一条是包元数据，只能红
        recipeHits = recipe_license_assignments()
        if not recipeHits:
            problems.append(
                "判据在 packaging/conan/conanfile.py 里没找到 license 字段：这一路的真值缺位"
            )
        elif len(recipeHits) > 1:
            problems.append(
                f"packaging/conan/conanfile.py 里命中 {len(recipeHits)} 处 license 赋值，"
                "判据无法认定哪一处是包元数据"
            )
        elif recipeHits[0] != licenseId:
            problems.append(f"Conan 配方许可 {recipeHits[0]!r} 与 LICENSE 判型 {licenseId!r} 不同解")

        if problems:
            # 失败时连校验和文件一起收掉：留着它是旧产物的残凭据，下游按 SHA256SUMS 核验时
            # 会把一个已被删除（或压根不完整）的 SBOM 读成「通过」。
            target.unlink(missing_ok=True)
            checksumFile.unlink(missing_ok=True)
            print("SBOM 自检失败：" + "；".join(problems), file=sys.stderr)
            return 1

        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        checksumFile.write_text(f"{digest}  {target.name}\n", encoding="utf-8", newline="\n")
    except BaseException:
        # 任何非零出口都先收掉产物，再让异常照原样冒出去
        target.unlink(missing_ok=True)
        checksumFile.unlink(missing_ok=True)
        raise

    print(f"已生成 {target}（{len(parsedComponents)} 个组件，版本 {version}，提交 {revision[:8]}）")
    print(f"SHA256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
