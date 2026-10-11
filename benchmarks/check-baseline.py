"""性能门禁：把 soak*.py --json-out 产出的实测结果与 benchmarks/baseline.json 逐项比对，越界即非零退出。

用法：
    python benchmarks/check-baseline.py <实测1.json> [实测2.json ...]
                                    [--baseline benchmarks/baseline.json]
                                    [--minimum-throughput-ratio 0.6]
                                    [--maximum-p50-ratio 1.5]
                                    [--maximum-p95-ratio 2.0]
                                    [--maximum-p99-ratio 2.5]
                                    [--maximum-working-set-ratio 1.5]

判定口径：
    · 实测 JSON 里的 failures 必须为 0（协议不变式与负载失败都算）——先修正确性，再看性能；
    · 同一个指标名给了多份实测时取**中位数**，baseline.json 里的数也是同口径的中位数，两侧一致。
      （中位数而不是最优值：最优值两边都得跟着「取最优」才公平，而单跑一次时最优值就是那一次本身，
      于是噪声直接进判定——实测里 h2c 32 条一批的 p50 在三次之间是 362/735/778us，按最优比会把
      一次正常发挥判成 2.03 倍退化。）**建议每次传 3 份以上的实测**，只有 1 份时中位数就是那一次。
    · 吞吐：不得低于基线的 --minimum-throughput-ratio 倍（默认 0.6）；
    · 每一档分位（p50 / p95 / p99）：不得高于基线的对应 `--maximum-<档>-ratio` 倍，默认 1.5 / 2.0 / 2.5；
      倍数按「分位越靠后越抖」递增。`maximumMicroseconds` 只录不判——单次调度抖动就能让它翻倍，
      判它等于判机器状态；
    · 基线里有、实测里没有的项判失败（漏测等于没测）；实测里多出来的项只提示。
    · 两套基线同吃：soak 基线（每项带 p50/p95/p99 分位键）按上面那条判；微基准那种「点式」基线
      （每项只有 throughputPerSecond/nanosecondsPerOperation）只判吞吐，不套「缺分位档即红」——
      那条规则只服务于 soak 侧的分位补录历史，套到点式上会把 microbench 那条入口变成恒红。
    · `connection-sweep-*` 档（并发扫描）判的是工作集峰值这个容量规划量（键 workingSetPeakKib）：
      基线录了该档就按 --maximum-working-set-ratio 判，没录只打印读数、不硬造阈值；建成数与档内
      失败由文档级 failures 统一判（它们本就计进那份账），这里不重复。工作集峰值是绝对值量，
      没有基线值就没有参照系——「不判」与「判过」要分得开。

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

# `maximumMicroseconds` 刻意不在判定面上：它是整段最坏的一次，单次调度抖动就能翻倍，
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


def compareSweepTier(name: str, expected: dict | None, samples: dict, arguments) -> list:
    """连接数扫描档的判定：工作集峰值。

    扫描档判的不是吞吐与分位，而是「并发 N 条连接时服务端驻留多少」——容量规划看的就是这个量。
    它是绝对值读数，没有基线值就没有参照系，所以基线录了 workingSetPeakKib 才判；没录时只打印
    读数并标明「只落盘不判」——把「这一档还没进基线」如实说出来，而不是硬造一个阈值或静默跳过。
    建成数与档内失败不在这里判：它们计进文档级 failures，由「实测本身报告 N 条失败」统一拦。
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
    # 每一档分位一个上限倍数参数，默认值取自 LATENCY_RATIO_DEFAULTS（同一处真源）
    for latencyKey, defaultRatio in LATENCY_RATIO_DEFAULTS.items():
        parser.add_argument(f"--maximum-{latencyKey[:-len('Microseconds')].lower()}-ratio", type=float, default=defaultRatio,
                            dest=f"maximum_{latencyKey[:-len('Microseconds')].lower()}_ratio",
                            help=f"{latencyKey[:-len('Microseconds')]} 上限倍数，默认 {defaultRatio}")
    parser.add_argument("--maximum-working-set-ratio", type=float, default=1.5,
                        help="connection-sweep 档工作集峰值上限倍数，默认 1.5（基线录了该档才生效）")
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
