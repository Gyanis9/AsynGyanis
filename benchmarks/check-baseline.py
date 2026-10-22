"""性能门禁：把 soak*.py --json-out 的实测与 benchmarks/baseline.json 逐项比对，越界即非零退出。
用法：python benchmarks/check-baseline.py <实测.json ...> [--baseline PATH]
      [--minimum-throughput-ratio 0.6] [--maximum-p50-ratio 1.5] [--maximum-p95-ratio 2.0]
      [--maximum-p99-ratio 2.5] [--maximum-working-set-ratio 1.5]
判据：failures 为 0；多份实测同一指标取中位数（不是最优值，噪声会直接进判定）；
      吞吐不低于基线 0.6 倍；p50/p95/p99 不超过 1.5/2.0/2.5 倍；maximumMicroseconds 只录不判；
      基线有而实测缺的项判失败，多出的项只提示；点式基线只判吞吐；connection-sweep-* 判峰值。
退出码：0 全部在阈值内；1 有违反项；2 基线或实测读不了（前置缺件，不是回归结论）。
本机门禁：共享 runner 噪声不可控，CI 只跑正确性；改动前后同机留基线再比对。
"""

import argparse
import json
import os
import sys

# Windows runner 控制台默认 cp1252：中文正文在 print 时抛 UnicodeEncodeError，只剩 traceback。
# 与其它门禁脚本同形状，进来先把两个流钉成 UTF-8。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

DEFAULT_BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "baseline.json")

# maximumMicroseconds 刻意不在判定面上：它是整段最坏的一次，单次调度抖动就能翻倍，
# 判它等于判机器状态。分位这条线判到 p99 为止，max 只作描述性读数
GAUGED_KEYS = ("throughputPerSecond", "p50Microseconds", "p95Microseconds", "p99Microseconds")
DESCRIPTIVE_KEYS = ("connections", "pipeline", "requests", "throughputUnit")
# 每个受判定分位的默认上限倍数。新增一档只要在这里加一行，参数与循环都跟着长——
# 避免出现「测量脚本已经产出 p99，而门禁只认 p50/p95」这种两头各写一份的漂移
LATENCY_RATIO_DEFAULTS = {"p50Microseconds": 1.5, "p95Microseconds": 2.0, "p99Microseconds": 2.5}
# 连接数扫描档（soak.py 的 connection-sweep-<档位>）不走分位那套，判工作集峰值，见 compareSweepTier
SWEEP_NAME_PREFIX = "connection-sweep-"


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
    # 扫描档的工作集峰值同理攒样本；它不在 GAUGED_KEYS 里（单位 KiB，判定口径见 compareSweepTier）
    if "workingSetPeakKib" in incoming:
        samples.setdefault("workingSetPeakKib", []).append(incoming["workingSetPeakKib"])
    samples.setdefault("sources", []).append(sourcePath)
    for key in DESCRIPTIVE_KEYS:
        if key in incoming:
            samples.setdefault(key, incoming[key])
    return samples


# 跨运行离散度上限：同一指标各次实测的 max/min 超过它，量的是机器状态而不是被测代码。
# 本机会整段落进 ~1.5 倍负载区，故只做提示、不判失败，避免把机器噪声当成代码回归。
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


def compareSweepTier(name: str, expected: dict | None, samples: dict, arguments) -> list:
    """连接数扫描档判工作集峰值（workingSetPeakKib），容量规划看的就是并发 N 条连接时驻留多少。

    基线录了该档才判（绝对值量，没基线就没参照系），没录只打印读数；建成数与档内失败由
    文档级 failures 统一判，这里不重复。
    """
    violations = []
    runCount = len(samples["sources"])
    print(f"  -- {name}（{runCount} 份实测）")
    peakSamples = samples.get("workingSetPeakKib", [])
    expectedPeak = (expected or {}).get("workingSetPeakKib")
    if expectedPeak and not peakSamples:
        print(f"     [FAIL] 基线要求判定工作集峰值，实测里没有 workingSetPeakKib（采集那次没带 --pid？）")
        violations.append(f"{name}：基线要求判定工作集峰值，实测里没有 workingSetPeakKib 样本")
    elif expectedPeak:
        actualPeak = median(peakSamples)
        maximum = expectedPeak * arguments.maximum_working_set_ratio
        passed = actualPeak <= maximum
        print(f"     [{'OK  ' if passed else 'FAIL'}] 工作集峰值 {actualPeak:.0f}KiB / 基线 {expectedPeak:.0f}KiB"
              f" = {actualPeak / expectedPeak:.2f}x（上限 {arguments.maximum_working_set_ratio:.2f}x，"
              f"样本 {'/'.join(f'{value:.0f}' for value in peakSamples)}）")
        if not passed:
            violations.append(f"{name}：工作集峰值 {actualPeak:.0f}KiB 高于基线的 {arguments.maximum_working_set_ratio:.2f} 倍"
                              f"（上限 {maximum:.0f}KiB）")
    elif peakSamples:
        print(f"     [记录] 工作集峰值 {median(peakSamples):.0f}KiB（基线未录这一档的峰值，只落盘不判）")
    else:
        print(f"     [记录] 这一档既没有基线值也没有工作集峰值读数（采集那次没带 --pid？）")
    return violations


def compareMeasurement(name: str, expected: dict, samples: dict, arguments) -> list:
    """比对单项：返回违反项的中文描述列表（空列表表示通过）。"""
    if name.startswith(SWEEP_NAME_PREFIX):
        return compareSweepTier(name, expected, samples, arguments)
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

    # 点式基线（每项只有 nanosecondsPerOperation/throughputPerSecond 的微基准形态）没有分位键：
    # 对它跑「基线缺档即红」那一组会把 81 项 × 3 键全报成违反，完美复现也恒红。那条规则只服务
    # soak 基线（p99 刚加进测量侧、基线还没录它时得当场催补录），点式这一支只判吞吐
    if "nanosecondsPerOperation" in expected:
        if expected.get("throughputPerSecond"):
            print(f"     [点式基线] 只判吞吐；基线登记的纳秒/次 {expected['nanosecondsPerOperation']:.1f} ns 作描述性读数")
        else:
            # 与上面 soak 侧同一条规矩：一道判据都没有的基线项要当场出声，不许静默通过
            print(f"     [FAIL] 点式基线里没有 throughputPerSecond：这一档没人判，得补录")
            violations.append(f"{name}：点式基线缺少 throughputPerSecond，这道门对该档从未生效")
    else:
        for key, ratioLimit, label in ((key, getattr(arguments, f"maximum_{key[:-len('Microseconds')].lower()}_ratio"),
                                        key[:-len('Microseconds')]) for key in LATENCY_RATIO_DEFAULTS):
            expectedValue = expected.get(key)
            latencySamples = samples.get(key)
            if not expectedValue:
                # 基线里缺这一档＝这道判据从未生效，得当场上报而不是静默跳过：
                # p99 刚加进测量侧时，基线还没录它，「门禁判 p99」那句话就是空的
                print(f"     [FAIL] 基线里没有 {key}：这一档没人判，得补录")
                violations.append(f"{name}：基线缺少 {key}，这道门对该档从未生效")
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

    # 可信度自检放在判定之后：它不改变本项的输赢，只标出「这一批的中位数不值得信」
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
    # 每一档分位一个上限倍数参数，默认值取自 LATENCY_RATIO_DEFAULTS（同一处真源）
    for latencyKey, defaultRatio in LATENCY_RATIO_DEFAULTS.items():
        parser.add_argument(f"--maximum-{latencyKey[:-len('Microseconds')].lower()}-ratio", type=float, default=defaultRatio,
                            dest=f"maximum_{latencyKey[:-len('Microseconds')].lower()}_ratio",
                            help=f"{latencyKey[:-len('Microseconds')]} 上限倍数，默认 {defaultRatio}")
    parser.add_argument("--maximum-working-set-ratio", type=float, default=1.5,
                        help="connection-sweep 档工作集峰值上限倍数，默认 1.5（基线录了该档才生效）")
    arguments = parser.parse_args()

    try:
        baseline = loadJson(arguments.baseline)
    except (OSError, json.JSONDecodeError) as error:
        print(f"门禁无法执行：基线 {arguments.baseline} 读不了（{error}）——属前置缺件，不是回归结论", file=sys.stderr)
        return 2
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
        try:
            document = loadJson(path)
        except (OSError, json.JSONDecodeError) as error:
            print(f"门禁无法执行：实测 {path} 读不了（{error}）——属前置缺件，不是回归结论", file=sys.stderr)
            return 2
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
        # 扫描档即使没进基线也要走一遍判据处理：工作集峰值要打印读数、带上 --pid 的读数才看得见
        if name.startswith(SWEEP_NAME_PREFIX):
            violations.extend(compareSweepTier(name, None, measured[name], arguments))
        else:
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
