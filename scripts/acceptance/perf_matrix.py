"""容量、长稳、开销对与基线的测量编排。"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shlex
import signal
import statistics
import subprocess
import sys
import time
from contextlib import ExitStack
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]


def percentile(values: list[float], percent: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(percent / 100.0 * len(ordered)) - 1))
    return ordered[index]


def to_microseconds(value: str) -> float:
    text = value.strip().lower()
    if text.endswith("ms"):
        return float(text[:-2]) * 1000.0
    if text.endswith("us"):
        return float(text[:-2])
    if text.endswith("s"):
        return float(text[:-1]) * 1_000_000.0
    return float(text)


def finite_nonnegative(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0


def parse_wrk(text: str) -> dict:
    reported = re.search(r"WRK_JSON (\{.*\})", text)
    if reported:
        payload = json.loads(reported.group(1))
        sample = {key: payload[key] for key in ("throughput", "p50", "p95", "p99", "errors")}
        if not all(finite_nonnegative(value) for value in sample.values()):
            raise ValueError("wrk 输出了无效读数")
        sample.update(validSamples=int(sample["throughput"] > 0), percentileScope="client-histogram")
        return sample
    rate = re.search(r"Requests/sec:\s+([\d.]+)", text)
    if rate is None:
        raise ValueError("wrk 没有输出吞吐")
    latencies = {}
    for percent, value in re.findall(r"^\s*(50|95|99)%\s+([\d.]+(?:us|ms|s))\s*$", text, re.MULTILINE):
        latencies[int(percent)] = to_microseconds(value)
    sockets = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", text)
    statuses = re.search(r"Non-2xx or 3xx responses:\s*(\d+)", text)
    errors = sum(map(int, sockets.groups())) if sockets else 0
    errors += int(statuses.group(1)) if statuses else 0
    return {"throughput": float(rate.group(1)), "p50": latencies.get(50, 0.0),
            "p95": latencies.get(95, 0.0), "p99": latencies.get(99, 0.0), "errors": errors,
            "validSamples": int(len(latencies) == 3), "percentileScope": "client-reported"}


def parse_h2load_log(text: str) -> dict:
    values = []
    errors = 0
    for line in text.splitlines():
        fields = line.split("\t")
        if len(fields) < 3:
            continue
        try:
            duration = float(fields[2])
            status = int(fields[1])
        except ValueError:
            continue
        if not 200 <= status < 400:
            errors += 1
        elif finite_nonnegative(duration):
            values.append(duration)
    summary = re.search(
        r"requests:\s+\d+ total,\s+\d+ started,\s+\d+ done,\s+(\d+) succeeded,\s+(\d+) failed,\s+(\d+) errored,\s+(\d+) timeout",
        text,
    )
    if summary:
        errors += sum(int(value) for value in summary.groups()[1:])
    return {"p50": percentile(values, 50), "p95": percentile(values, 95), "p99": percentile(values, 99),
            "errors": errors, "validSamples": len(values) or (int(summary.group(1)) if summary else 0),
            "percentileScope": "request-log" if values else "unavailable-without-request-log"}


def aggregate_json_instances(instances: list[dict], errors: int = 0) -> dict:
    return {
        "throughput": sum(item["throughput"] for item in instances),
        "p50": percentile([item["p50"] for item in instances], 50),
        "p95": percentile([item["p95"] for item in instances], 95),
        "p99": percentile([item["p99"] for item in instances], 99),
        "errors": errors + sum(item["errors"] for item in instances),
        "validSamples": sum(item.get("validSamples", 0) for item in instances),
        "percentileScope": "percentiles-of-client-percentiles",
    }


def parse_h2load_rate(text: str) -> float:
    match = re.search(r"finished in [^\n,]+,\s*([\d.]+) req/s", text)
    if match is None:
        match = re.search(r"(?:req/s|requests/s)\s*:\s*([\d.]+)", text)
    return float(match.group(1)) if match else 0.0


def parse_json_client(text: str, messages: int) -> dict:
    lines = [line for line in text.splitlines() if line.strip().startswith("{")]
    if not lines:
        raise ValueError("负载探针没有输出 JSON 行")
    payload = json.loads(lines[-1])
    required = ("messages", "elapsed_ms", "p50_us", "p95_us", "p99_us", "errors")
    if not all(finite_nonnegative(payload.get(key)) for key in required) or payload["elapsed_ms"] <= 0:
        raise ValueError("负载探针缺少有效读数")
    return {
        "throughput": payload["messages"] / (payload["elapsed_ms"] / 1000.0),
        "p50": payload["p50_us"], "p95": payload["p95_us"], "p99": payload["p99_us"],
        "errors": payload["errors"], "validSamples": payload["messages"],
        "percentileScope": "client-reported",
    }


def detect_saturation(points: list[dict]) -> int | None:
    for index in range(1, len(points)):
        previous = points[index - 1]
        current = points[index]
        gain = current["throughput"] / previous["throughput"] if previous["throughput"] else 0.0
        p99_ratio = current["p99"] / previous["p99"] if previous["p99"] else 0.0
        if gain < 1.05 and (p99_ratio > 2.0 or current["errors"] > 0):
            return index
    return None


def sample_resources(pid: int) -> dict:
    result = {"fds": None, "rssBytes": None, "valid": False, "failureReasons": []}
    try:
        result["fds"] = len(os.listdir(f"/proc/{pid}/fd"))
        fields = Path(f"/proc/{pid}/statm").read_text(encoding="ascii").split()
        result["rssBytes"] = int(fields[1]) * os.sysconf("SC_PAGE_SIZE")
        if result["fds"] <= 0 or result["rssBytes"] <= 0:
            raise ValueError("进程资源读数为空")
        result["valid"] = True
    except (OSError, ValueError, IndexError, AttributeError) as error:
        result["failureReasons"].append(f"invalid-resource: pid={pid}: {error}")
    return result


def assess_stability(samples: list[dict], loop_count: int) -> dict:
    failures = []
    result = {"failureReasons": failures}
    if not samples or any(
        not sample.get("valid", True)
        or not finite_nonnegative(sample.get("time"))
        or any(not finite_nonnegative(sample.get(key)) or sample[key] <= 0 for key in ("fds", "rssBytes"))
        for sample in samples
    ):
        failures.append("invalid-resource: 缺少有效的进程资源样本")
        return result
    if samples[0].get("phase") != "baseline" or samples[-1].get("phase") != "quiet":
        failures.append("invalid-resource: 缺少启动空载或结束静默样本")
        return result
    if loop_count < 1 or samples[-1]["fds"] > samples[0]["fds"] + 4 * loop_count:
        failures.append("fd-drift: 静默后 fd 超过启动空载基线 +4/loop")
    load = [sample for sample in samples if sample.get("phase") == "load"]
    steady = [sample for sample in load if sample["time"] >= 600]
    if (len(steady) < 121 or steady[0]["time"] > 611 or steady[-1]["time"] < 1800
            or steady[-1]["time"] - steady[0]["time"] < 1199
            or any(not 0 < right["time"] - left["time"] <= 11 for left, right in zip(load, load[1:]))):
        failures.append("insufficient-duration: 需要预热 600 秒后至少 1200 秒的 10 秒间隔采样")
        return result
    warmup_rss = steady[0]["rssBytes"]
    times = [sample["time"] - steady[0]["time"] for sample in steady]
    mean_time = statistics.mean(times)
    mean_rss = statistics.mean(sample["rssBytes"] for sample in steady)
    slope = sum((second - mean_time) * (sample["rssBytes"] - mean_rss)
                for second, sample in zip(times, steady)) / sum((second - mean_time) ** 2 for second in times)
    hourly = slope * 3600
    result.update(warmupRssBytes=warmup_rss, rssSlopeBytesPerHour=hourly,
                  rssSlopeLimitBytesPerHour=warmup_rss * 0.005)
    if hourly > warmup_rss * 0.005 and not math.isclose(hourly, warmup_rss * 0.005, rel_tol=1e-9):
        failures.append("rss-slope: RSS 线性斜率超过预热值的 0.5%/小时")
    if max(steady[-1]["rssBytes"], samples[-1]["rssBytes"]) > warmup_rss * 1.10:
        failures.append("rss-end: RSS 末值超过预热值的 110%")
    return result


def is_stable(samples: list[dict], loop_count: int) -> bool:
    return not assess_stability(samples, loop_count)["failureReasons"]


def ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator else 0.0


def compare_baseline(current: dict, baseline: dict) -> list[str]:
    failures: list[str] = []
    baseline_points = {point["concurrency"]: point for point in baseline.get("points", [])}
    for point in current.get("points", []):
        reference = baseline_points.get(point["concurrency"])
        if reference is None:
            failures.append(f"基线缺少并发点 {point['concurrency']}")
            continue
        if ratio(point["throughput"], reference["throughput"]) < 0.60:
            failures.append(f"并发 {point['concurrency']} 吞吐低于基线 0.60×")
        if ratio(point["p50"], reference["p50"]) > 1.50:
            failures.append(f"并发 {point['concurrency']} p50 高于基线 1.50×")
        if ratio(point["p95"], reference["p95"]) > 2.00:
            failures.append(f"并发 {point['concurrency']} p95 高于基线 2.00×")
        if ratio(point["p99"], reference["p99"]) > 2.50:
            failures.append(f"并发 {point['concurrency']} p99 高于基线 2.50×")
    return failures


def render(template: str, values: dict) -> str:
    result = template
    for key, value in values.items():
        result = result.replace("{" + key + "}", str(value))
    return result


def metric_failures(sample: dict) -> list[str]:
    failures = []
    if not finite_nonnegative(sample.get("errors")) or sample["errors"] != 0:
        failures.append("errors: 客户端报告请求失败或错误计数无效")
    if not finite_nonnegative(sample.get("throughput")) or sample["throughput"] <= 0:
        failures.append("no-throughput: 客户端没有有效吞吐")
    if not finite_nonnegative(sample.get("validSamples")) or sample["validSamples"] <= 0:
        failures.append("no-valid-samples: 客户端没有有效请求样本")
    return failures


def _kill_group(process) -> None:
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        elif process.poll() is None:
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                           capture_output=True, timeout=5, check=False)
            if process.poll() is None:
                process.kill()
    except ProcessLookupError:
        pass


def _measurement_values(case: dict, plan: dict, args, concurrency: int) -> dict:
    sampling = plan["sampling"]
    return {
        "concurrency": concurrency,
        "duration": args.duration or sampling["measureSeconds"],
        "messages": case.get("messages", 200),
        "url": case["url"],
        "build": str(args.build),
        "repo": str(REPO_ROOT),
    }


def _parse_case_output(case: dict, values: dict, output: str, log_path: Path | None) -> dict:
    if case["protocol"] == "wrk":
        return parse_wrk(output)
    if case["protocol"] == "h2load":
        log = log_path.read_text(encoding="utf-8") if log_path is not None and log_path.is_file() else ""
        sample = parse_h2load_log(log + "\n" + output)
        sample["throughput"] = parse_h2load_rate(output)
        return sample
    return parse_json_client(output, values["messages"])


def _run_load(case: dict, values: dict, args, prefix: str, sample_resource=None, required_seconds: float = 0) -> dict:
    command = render(case["command"], values)
    log_path = None
    if case["protocol"] == "h2load" and case.get("log_requests", True):
        log_path = args.out / f"h2load-{prefix}.log"
        log_path.write_text("", encoding="utf-8")
        quoted = shlex.quote(str(log_path)) if os.name == "posix" else subprocess.list2cmdline([str(log_path)])
        command += f" --log-file={quoted}"
    count = int(values["concurrency"]) if case["protocol"] == "json" else 1
    started = time.monotonic()
    timeout = min(float(args.timeout), float(values["duration"]) + (60 if required_seconds else 30))
    deadline = started + timeout
    next_sample = started
    processes = []
    clients = []
    failures = []
    completed = False
    with ExitStack() as files:
        try:
            for index in range(count):
                path = args.out / f"{prefix}-client-{index}.log"
                stream = files.enter_context(path.open("wb"))
                client_started = time.monotonic()
                options = {"start_new_session": True} if os.name == "posix" else {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
                process = subprocess.Popen(command, shell=True, stdout=stream, stderr=subprocess.STDOUT, **options)
                client = {"pid": process.pid, "outputPath": str(path), "returncode": None,
                          "startedSeconds": client_started - started, "seconds": 0.0, "failureReasons": []}
                processes.append((process, client))
                clients.append(client)
            while processes:
                now = time.monotonic()
                if sample_resource is not None and now >= next_sample:
                    sample_resource("load")
                    next_sample = started + (math.floor((now - started) / 10) + 1) * 10
                pending = []
                for process, client in processes:
                    if client["returncode"] is not None:
                        continue
                    code = process.poll()
                    if code is None:
                        pending.append((process, client))
                        continue
                    client["returncode"] = code
                    client["seconds"] = now - started - client["startedSeconds"]
                    if code != 0:
                        client["failureReasons"].append(f"nonzero-exit: 客户端退出码 {code}")
                    if client["seconds"] < required_seconds:
                        client["failureReasons"].append(f"early-exit: 实跑 {client['seconds']:.3f}s < {required_seconds}s")
                if not pending:
                    completed = True
                    break
                if now >= deadline:
                    for _, client in pending:
                        client["failureReasons"].append(f"timeout: 客户端超过本轮 {timeout}s 时限")
                    break
                pause = min(1.0, deadline - now)
                if sample_resource is not None:
                    pause = min(pause, max(0.001, next_sample - now))
                time.sleep(pause)
        except OSError as error:
            failures.append(f"client-process: {error}")
        finally:
            if not completed:
                for process, client in processes:
                    try:
                        _kill_group(process)
                    except (OSError, subprocess.TimeoutExpired) as error:
                        client["failureReasons"].append(f"cleanup-failed: {error}")
                for process, client in processes:
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        client["failureReasons"].append("cleanup-failed: 客户端没有退出")
                    if client["returncode"] is None:
                        client["returncode"] = process.returncode
                        client["seconds"] = time.monotonic() - started - client["startedSeconds"]
    seconds = time.monotonic() - started
    instances = []
    for client in clients:
        try:
            output = (args.out / f"{prefix}-client-{len(instances)}.log").read_text(encoding="utf-8", errors="replace")
            sample = _parse_case_output(case, values, output, log_path)
        except (OSError, ValueError, KeyError, TypeError, AttributeError, OverflowError) as error:
            sample = aggregate_json_instances([])
            client["failureReasons"].append(f"invalid-output: {error}")
        client["failureReasons"].extend(metric_failures(sample))
        client["metrics"] = sample
        instances.append(sample)
        failures.extend(f"client {client['pid']}: {reason}" for reason in client["failureReasons"])
    metrics = aggregate_json_instances(instances) if case["protocol"] == "json" or not instances else dict(instances[0])
    failures.extend(metric_failures(metrics))
    metrics.update(seconds=seconds, clients=clients, command=command, failureReasons=list(dict.fromkeys(failures)))
    return metrics


def run_capacity(case: dict, plan: dict, args) -> dict:
    sampling = plan["sampling"]
    runs = int(args.runs or sampling["runs"])
    points = []
    failures = []
    for concurrency in case["concurrency"]:
        samples = []
        for run_index in range(runs):
            values = _measurement_values(case, plan, args, concurrency)
            prefix = f"{case['name']}-{concurrency}-{run_index}"
            warmup = None
            if sampling.get("warmupSeconds", 0) > 0:
                warmup = _run_load(case, dict(values, duration=sampling["warmupSeconds"]), args, prefix + "-warmup")
                failures.extend(f"warmup {prefix}: {reason}" for reason in warmup["failureReasons"])
            sample = _run_load(case, values, args, prefix)
            if warmup is not None:
                sample["warmup"] = warmup
            samples.append(sample)
            failures.extend(f"concurrency {concurrency}, run {run_index}: {reason}" for reason in sample["failureReasons"])
            time.sleep(float(sampling.get("cooldownSeconds", 0)))
        if not samples:
            failures.append(f"no-valid-samples: 并发 {concurrency} 没有测量轮次")
            continue
        points.append({
            "concurrency": concurrency,
            "throughput": statistics.median(item["throughput"] for item in samples),
            "p50": statistics.median(item["p50"] for item in samples),
            "p95": statistics.median(item["p95"] for item in samples),
            "p99": statistics.median(item["p99"] for item in samples),
            "errors": sum(item["errors"] for item in samples),
            "percentileScope": "median-of-run-percentiles",
            "runs": samples,
        })
    if not points:
        failures.append("no-valid-samples: 没有容量测量点")
    return {"case": case["name"], "points": points, "saturationIndex": detect_saturation(points),
            "failureReasons": failures}


def run_soak(case: dict, plan: dict, args) -> dict:
    values = _measurement_values(case, plan, args, case["concurrency"])
    values["duration"] = case["durationSeconds"]
    values["messages"] = case.get("messages", 0)
    resources = []
    started = time.monotonic()
    resource_path = args.out / f"{case['name']}-resources.jsonl"
    with resource_path.open("w", encoding="utf-8") as resource_log:
        def record(phase):
            sample = sample_resources(args.server_pid)
            sample.update(time=time.monotonic() - started, phase=phase)
            resources.append(sample)
            resource_log.write(json.dumps(sample, ensure_ascii=False) + "\n")
            resource_log.flush()

        record("baseline")
        metrics = _run_load(case, values, args, f"{case['name']}-soak", record, float(values["duration"]))
        quiet_seconds = max(5.0, float(plan["sampling"].get("cooldownSeconds", 5)))
        time.sleep(quiet_seconds)
        record("quiet")
    loop_count = getattr(args, "loop_count", 1)
    stability = assess_stability(resources, loop_count)
    failures = metrics["failureReasons"] + stability["failureReasons"]
    if float(values["duration"]) < 1800 or metrics["seconds"] < float(values["duration"]):
        failures.append("insufficient-duration: 长稳必须实跑至少计划时长且不少于 1800 秒")
    return {
        "case": case["name"], "metrics": metrics, "resources": resources,
        "resourcesJsonl": str(resource_path), "seconds": metrics["seconds"],
        "requiredSeconds": values["duration"], "quietSeconds": quiet_seconds, "loopCount": loop_count,
        "stability": stability, "failureReasons": failures, "stable": not failures,
    }


def run_overhead(case: dict, plan: dict, args) -> dict:
    results = {}
    for label, overrides in case["variants"].items():
        variant = dict(case)
        variant.update(overrides)
        variant["name"] = f"{case['name']}-{label}"
        results[label] = run_capacity(variant, plan, args)
    failures = [f"variant {label}: {reason}" for label, result in results.items() for reason in result["failureReasons"]]
    if not results:
        failures.append("no-valid-samples: 没有开销对测量变体")
    return {"case": case["name"], "variants": results, "failureReasons": failures}


def main() -> int:
    parser = argparse.ArgumentParser(description="性能容量、长稳、开销对与基线")
    parser.add_argument("--plan", required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--build", default="build/release")
    parser.add_argument("--out", required=True)
    parser.add_argument("--runs", type=int, default=0)
    parser.add_argument("--duration", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=3600.0)
    parser.add_argument("--server-pid", type=int, default=0)
    parser.add_argument("--loop-count", type=int, default=1)
    parser.add_argument("--baseline-in", default="")
    parser.add_argument("--write-baseline", default="")
    args = parser.parse_args()
    args.build = Path(args.build)
    args.out = Path(args.out).resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    plan = yaml.safe_load(Path(args.plan).read_text(encoding="utf-8"))
    case = plan["cases"][args.case]
    case["name"] = args.case
    if case["type"] == "capacity":
        result = run_capacity(case, plan, args)
    elif case["type"] == "soak":
        result = run_soak(case, plan, args)
    elif case["type"] == "overhead":
        result = run_overhead(case, plan, args)
    else:
        print(f"不认识的用例类型：{case['type']}", file=sys.stderr)
        return 2
    if args.baseline_in and case["type"] == "capacity":
        baseline = json.loads(Path(args.baseline_in).read_text(encoding="utf-8"))
        result["baselineFailures"] = compare_baseline(result, baseline)
        result["failureReasons"].extend(result["baselineFailures"])
    result["verdict"] = "red" if result["failureReasons"] else "green"
    result["measuredAt"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    result["commit"] = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, check=False).stdout.strip()
    output_path = args.out / f"{args.case}.json"
    output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    if args.write_baseline:
        Path(args.write_baseline).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"性能结果：{output_path}")
    return 1 if result.get("verdict") == "red" else 0


if __name__ == "__main__":
    raise SystemExit(main())
