"""性能门禁：把 soak*.py --json-out 产出的实测结果与 benchmarks/baseline.json 逐项比对，越界即非零退出。

用法：
    python benchmarks/check-baseline.py <实测1.json> [实测2.json ...]
                                    [--baseline benchmarks/baseline.json]
                                    [--minimum-throughput-ratio 0.6]
                                    [--maximum-p50-ratio 1.5]
                                    [--maximum-p95-ratio 2.0]

判定口径：
    · 实测 JSON 里的 failures 必须为 0（协议不变式与负载失败都算）——先修正确性，再看性能；
    · 同一个指标名给了多份实测时取**中位数**，baseline.json 里的数也是同口径的中位数，两侧一致。
      （中位数而不是最优值：最优值两边都得跟着「取最优」才公平，而单跑一次时最优值就是那一次本身，
      于是噪声直接进判定——实测里 h2c 32 条一批的 p50 在三次之间是 362/735/778us，按最优比会把
      一次正常发挥判成 2.03 倍退化。）**建议每次传 3 份以上的实测**，只有 1 份时中位数就是那一次。
    · 吞吐：不得低于基线的 --minimum-throughput-ratio 倍（默认 0.6）；
    · p50 / p95：不得高于基线的 --maximum-p50-ratio（默认 1.5）/ --maximum-p95-ratio（默认 2.0）倍；
    · 基线里有、实测里没有的项判失败（漏测等于没测）；实测里多出来的项只提示。

**这道门禁能抓什么、不能抓什么**：它抓的是量级回归；抓不住 10% 级的漂移——那需要更严谨的测量方法
（固定 CPU 频率、绑核、关后台负载、多轮取中位数），不是脚本能补的。默认阈值是按本机实测噪声定的：
同一份 Release 代码连跑四次，h1 保持连接吞吐 24.8k~30.8k、h2c 32 条一批 38k~58k（约 1.5 倍）。
**为什么是「本机门禁」而不是 CI 门禁**：共享 runner 的 CPU 型号、频率与邻居负载都不可控，
跨机器比较会把机器噪声当成代码回归。CI 只跑正确性（windows-ci.yml / linux-ci.yml）；
这道门禁的正确用法是「改动前在同一台机器上留一份基线，改动后重跑并比对」，由跑的人保证环境一致。
"""

import argparse
import json
import os
import sys

DEFAULT_BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "baseline.json")

LATENCY_KEYS = ("p50Microseconds", "p95Microseconds", "maximumMicroseconds")
GAUGED_KEYS = ("throughputPerSecond",) + LATENCY_KEYS
DESCRIPTIVE_KEYS = ("connections", "pipeline", "requests", "throughputUnit")


def loadJson(path: str) -> dict:
    """读一个 UTF-8 JSON 文件（找不到或格式错时抛出，交给调用方报错退出）。"""
    with open(path, "r", encoding="utf-8") as stream:
        return json.load(stream)


def median(values: list) -> float:
    """取中位数：偶数个样本取中间两个的均值。"""
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2.0


def accumulateMeasurement(samples: dict, incoming: dict, sourcePath: str) -> dict:
    """把一次运行的结果收进该指标的样本表：受判定的指标攒样本，描述性字段取首次出现的值。"""
    for key in GAUGED_KEYS:
        if key in incoming:
            samples.setdefault(key, []).append(incoming[key])
    samples.setdefault("sources", []).append(sourcePath)
    for key in DESCRIPTIVE_KEYS:
        if key in incoming:
            samples.setdefault(key, incoming[key])
    return samples


# 跨运行离散度的可信上限：同一指标各次实测之间 max/min 超过这个倍数，说明这批读数量的是
# 「这次跑落在哪个区」而不是被测代码。本机实测会整段进入 ~1.5 倍的负载区——同一份二进制
# 连续三跑全在 1100 ns、隔一批又全在 736 ns，而组内离散只有 1.01，所以「分批各跑几轮」看不出来。
# 本会话有两次 0.78x 的假报红（qpack-decode、header-first-value）就是这么来的，故只做提示、
# 不判失败：判失败会把机器状态当成代码回归。
kDispersionRatioLimit = 1.3


def dispersionRatio(samples: dict) -> tuple:
    """跨运行离散度自检：返回 (最大 max/min 倍数, 对应指标名)；不足两份实测时返回 (1.0, "")。"""
    widestRatio = 1.0
    widestKey = ""
    for key in GAUGED_KEYS:
        values = samples.get(key)
        if not values or len(values) < 2:
            continue
        minimumValue = min(values)
        maximumValue = max(values)
        ratio = maximumValue / minimumValue if minimumValue > 0 else float("inf")
        if ratio > widestRatio:
            widestRatio = ratio
            widestKey = key
    return widestRatio, widestKey


def compareMeasurement(name: str, expected: dict, samples: dict, arguments) -> list:
    """比对单项：返回违反项的中文描述列表（空列表表示通过）。"""
    violations = []
    runCount = len(samples["sources"])
    print(f"  -- {name}（{runCount} 份实测取中位数）")

    expectedThroughput = expected.get("throughputPerSecond")
    throughputSamples = samples.get("throughputPerSecond")
    if expectedThroughput and not throughputSamples:
        print(f"     [FAIL] 基线要求判定吞吐，实测里没有 throughputPerSecond")
        violations.append(f"{name}：基线要求判定吞吐，实测里没有 throughputPerSecond 样本")
    elif expectedThroughput and throughputSamples:
        actualThroughput = median(throughputSamples)
        minimum = expectedThroughput * arguments.minimum_throughput_ratio
        passed = actualThroughput >= minimum
        print(f"     [{'OK  ' if passed else 'FAIL'}] 吞吐 {actualThroughput:.0f} / 基线 {expectedThroughput:.0f}"
              f" = {actualThroughput / expectedThroughput:.2f}x（下限 {arguments.minimum_throughput_ratio:.2f}x，"
              f"样本 {'/'.join(f'{value:.0f}' for value in throughputSamples)}）")
        if not passed:
            violations.append(f"{name}：吞吐 {actualThroughput:.0f} 低于基线的 {arguments.minimum_throughput_ratio:.2f} 倍"
                              f"（下限 {minimum:.0f}）")

    for key, ratioLimit, label in (("p50Microseconds", arguments.maximum_p50_ratio, "p50"),
                                   ("p95Microseconds", arguments.maximum_p95_ratio, "p95")):
        expectedValue = expected.get(key)
        latencySamples = samples.get(key)
        if not expectedValue:
            continue
        if not latencySamples:
            print(f"     [FAIL] 基线要求判定 {label}，实测里没有 {key}")
            violations.append(f"{name}：基线要求判定 {label}，实测里没有 {key} 样本")
            continue
        actualValue = median(latencySamples)
        maximum = expectedValue * ratioLimit
        passed = actualValue <= maximum
        print(f"     [{'OK  ' if passed else 'FAIL'}] {label} {actualValue:.1f}us / 基线 {expectedValue:.1f}us"
              f" = {actualValue / expectedValue:.2f}x（上限 {ratioLimit:.2f}x，"
              f"样本 {'/'.join(f'{value:.1f}' for value in latencySamples)}）")
        if not passed:
            violations.append(f"{name}：{label} {actualValue:.1f}us 高于基线的 {ratioLimit:.2f} 倍（上限 {maximum:.1f}us）")

    # 可信度自检放在判定之后：它不改变本项的输赢，只标出「这次的中位数不值得信」
    widestRatio, widestKey = dispersionRatio(samples)
    if widestRatio > kDispersionRatioLimit:
        print(f"     [噪声] {widestKey} 跨 {runCount} 份实测的 max/min = {widestRatio:.2f}x，"
              f"超过可信上限 {kDispersionRatioLimit:.2f}x：本项只当量级退化检查用，"
              f"亚阈值差异请以同批交替的 A/B 为准")

    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description="性能门禁：实测结果 vs 基线")
    parser.add_argument("measured", nargs="+", help="soak*.py --json-out 产出的 JSON（可多份，按 measurements 的键取中位数）")
    parser.add_argument("--baseline", default=DEFAULT_BASELINE, help=f"基线文件，默认 {DEFAULT_BASELINE}")
    parser.add_argument("--minimum-throughput-ratio", type=float, default=0.6, help="吞吐下限倍数，默认 0.6")
    parser.add_argument("--maximum-p50-ratio", type=float, default=1.5, help="p50 上限倍数，默认 1.5")
    parser.add_argument("--maximum-p95-ratio", type=float, default=2.0, help="p95 上限倍数，默认 2.0")
    arguments = parser.parse_args()

    baseline = loadJson(arguments.baseline)
    baselineBenchmarks = baseline.get("benchmarks", {})
    if not baselineBenchmarks:
        print(f"门禁无法执行：基线 {arguments.baseline} 里没有 benchmarks 段")
        return 1

    print(f"== 基线 {arguments.baseline} ==")
    print(f"   {baseline.get('build', '未标注构建')}，记录于 {baseline.get('recordedAt', '未知')}"
          f"，机器 {baseline.get('machine', '未标注')}")

    measured = {}
    violations = []
    for path in arguments.measured:
        document = loadJson(path)
        documentFailures = int(document.get("failures", 0))
        if documentFailures:
            violations.append(f"{path}：实测本身报告 {documentFailures} 条失败（先修正确性再看性能）")
        for name, values in document.get("measurements", {}).items():
            measured[name] = accumulateMeasurement(measured.setdefault(name, {}), values, path)

    print(f"== 比对（{len(arguments.measured)} 份实测，{len(measured)} 项指标）==")
    for name, expected in sorted(baselineBenchmarks.items()):
        samples = measured.get(name)
        if samples is None:
            print(f"  -- {name}：缺测")
            violations.append(f"{name}：实测结果里没有这一项（基线要求它存在）")
            continue
        violations.extend(compareMeasurement(name, expected, samples, arguments))

    for name in sorted(set(measured) - set(baselineBenchmarks)):
        print(f"  -- {name}：基线里没有这一项（来自 {measured[name]['sources'][0]}），仅提示不判失败")

    print(f"== 汇总：违反项 {len(violations)} 条 ==")
    for violation in violations:
        print(f"  FAIL: {violation}")
    if violations:
        return 1
    print("  OK: 全部指标在阈值内")
    return 0


if __name__ == "__main__":
    sys.exit(main())
