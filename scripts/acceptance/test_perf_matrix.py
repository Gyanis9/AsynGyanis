import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import perf_matrix


class PerfParserTest(unittest.TestCase):
    def test_percentile(self):
        self.assertEqual(perf_matrix.percentile([1, 2, 3, 4, 5], 50), 3)
        self.assertEqual(perf_matrix.percentile([1, 2, 3, 4, 5], 99), 5)

    def test_parse_wrk(self):
        text = """
Running 30s test @ http://127.0.0.1/bench
  2 threads and 8 connections
  Latency Distribution
     50%  131.90us
     95%  234.10us
     99%    1.20ms
Requests/sec: 54804.00
"""
        sample = perf_matrix.parse_wrk(text)
        self.assertAlmostEqual(sample["throughput"], 54804.0)
        self.assertAlmostEqual(sample["p50"], 131.9)
        self.assertAlmostEqual(sample["p99"], 1200.0)

    def test_parse_wrk_prefers_reported_percentiles(self):
        text = 'WRK_JSON {"throughput": 1234.5, "p50": 10.0, "p95": 20.0, "p99": 30.0, "errors": 1}\n'
        sample = perf_matrix.parse_wrk(text)
        self.assertEqual(sample["throughput"], 1234.5)
        self.assertEqual(sample["p95"], 20.0)
        self.assertEqual(sample["errors"], 1)

    def test_parse_h2load_log(self):
        log = "1000\t200\t120\n1100\t200\t130\n1200\t200\t140\n"
        sample = perf_matrix.parse_h2load_log(log)
        self.assertEqual(sample["p50"], 130.0)
        self.assertEqual(sample["p99"], 140.0)

    def test_aggregate_json_instances(self):
        instances = [
            {"throughput": 100.0, "p50": 10.0, "p95": 20.0, "p99": 30.0, "errors": 0},
            {"throughput": 50.0, "p50": 20.0, "p95": 30.0, "p99": 40.0, "errors": 1},
        ]
        result = perf_matrix.aggregate_json_instances(instances, errors=2)
        self.assertEqual(result["throughput"], 150.0)
        self.assertEqual(result["errors"], 3)

    def test_parse_h2load_rate(self):
        self.assertEqual(perf_matrix.parse_h2load_rate("req/s           :    9490.76"), 9490.76)
        self.assertEqual(perf_matrix.parse_h2load_rate("requests/s: 1234.5"), 1234.5)
        self.assertEqual(perf_matrix.parse_h2load_rate("no rate here"), 0.0)

    def test_h2load_total_rate_takes_precedence_over_per_client_table(self):
        text = "finished in 30.00s, 1234.50 req/s, 12.00MB/s\nreq/s : 100.00 200.00 150.00\n"
        self.assertEqual(perf_matrix.parse_h2load_rate(text), 1234.5)

    def test_parse_h2load_summary_errors_without_log(self):
        text = "requests: 100 total, 100 started, 100 done, 90 succeeded, 6 failed, 4 errored, 0 timeout\n"
        sample = perf_matrix.parse_h2load_log(text)
        self.assertEqual(sample["errors"], 10)

    def test_detect_saturation(self):
        points = [
            {"concurrency": 1, "throughput": 100.0, "p99": 100.0, "errors": 0},
            {"concurrency": 8, "throughput": 400.0, "p99": 200.0, "errors": 0},
            {"concurrency": 32, "throughput": 415.0, "p99": 500.0, "errors": 0},
        ]
        index = perf_matrix.detect_saturation(points)
        self.assertEqual(index, 2)

    def test_parse_json_client_line(self):
        text = 'noise\n{"messages": 200, "errors": 0, "p50_us": 120.0, "p95_us": 200.0, "p99_us": 300.0, "elapsed_ms": 1000}\n'
        sample = perf_matrix.parse_json_client(text, messages=200)
        self.assertEqual(sample["throughput"], 200.0)
        self.assertEqual(sample["p99"], 300.0)

    def test_json_must_not_invent_successful_messages(self):
        with self.assertRaises(ValueError):
            perf_matrix.parse_json_client('{"elapsed_ms": 1000}', messages=200)

    def test_json_invalid_numeric_metrics_are_rejected(self):
        for field, value in (("elapsed_ms", 0), ("messages", -1), ("p99_us", float("nan"))):
            payload = json.loads(client_output("json").splitlines()[-1])
            payload[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                perf_matrix.parse_json_client(json.dumps(payload), messages=200)

    def test_wrk_text_errors_are_not_ignored(self):
        text = ("Requests/sec: 100\n 50% 10us\n 95% 20us\n 99% 30us\n"
                "Socket errors: connect 1, read 2, write 3, timeout 4\n"
                "Non-2xx or 3xx responses: 5\n")
        self.assertEqual(perf_matrix.parse_wrk(text)["errors"], 15)

    def test_h2load_summary_supplies_request_count_but_not_fake_percentiles(self):
        sample = perf_matrix.parse_h2load_log(
            "requests: 100 total, 100 started, 100 done, 100 succeeded, 0 failed, 0 errored, 0 timeout")
        self.assertEqual(sample.get("validSamples"), 100)
        self.assertEqual(sample.get("percentileScope"), "unavailable-without-request-log")


def resource_series():
    return ([{"phase": "baseline", "time": 0, "fds": 100, "rssBytes": 1000}]
            + [{"phase": "load", "time": second, "fds": 300, "rssBytes": 1000}
               for second in range(0, 1801, 10)]
            + [{"phase": "quiet", "time": 1805, "fds": 104, "rssBytes": 1000}])


class PerfVerdictTest(unittest.TestCase):
    def test_fd_drift_allows_four_per_loop(self):
        samples = resource_series()
        self.assertTrue(perf_matrix.is_stable(samples, loop_count=1))
        samples[-1]["fds"] = 108
        self.assertTrue(perf_matrix.is_stable(samples, loop_count=2))
        self.assertFalse(perf_matrix.is_stable(samples, loop_count=1))

    def test_rss_drift_rejects_ten_percent_growth(self):
        samples = resource_series()
        samples[-2]["rssBytes"] = 1101
        self.assertFalse(perf_matrix.is_stable(samples, loop_count=1))

    def test_quiet_rss_is_also_an_end_measurement(self):
        samples = resource_series()
        samples[-1]["rssBytes"] = 1101
        self.assertFalse(perf_matrix.is_stable(samples, loop_count=1))

    def test_slow_growth_below_ten_percent_still_fails_slope(self):
        samples = resource_series()
        for sample in samples:
            if sample["phase"] == "load" and sample["time"] >= 600:
                sample["rssBytes"] = 1000 + (sample["time"] - 600) / 120
        self.assertFalse(perf_matrix.is_stable(samples, loop_count=1))

    def test_warmup_growth_is_excluded_from_regression(self):
        samples = resource_series()
        for sample in samples[1:]:
            sample["rssBytes"] = 1000 + min(sample["time"], 600) * 10
        self.assertTrue(perf_matrix.is_stable(samples, loop_count=1))

    def test_growth_at_half_percent_per_hour_is_allowed(self):
        samples = resource_series()
        for sample in samples:
            if sample["phase"] == "load" and sample["time"] >= 600:
                sample["rssBytes"] += (sample["time"] - 600) * 5 / 3600
        self.assertTrue(perf_matrix.is_stable(samples, loop_count=1))

    def test_missing_zero_or_sparse_resources_cannot_pass(self):
        for index in (0, 90, -1):
            with self.subTest(index=index):
                samples = resource_series()
                samples[index].update(fds=0, rssBytes=0)
                self.assertFalse(perf_matrix.is_stable(samples, loop_count=1))
        self.assertFalse(perf_matrix.is_stable(resource_series()[:-1], loop_count=1))
        sparse = [sample for sample in resource_series()
                  if sample["phase"] != "load" or sample["time"] % 60 == 0]
        self.assertFalse(perf_matrix.is_stable(sparse, loop_count=1))

    def test_resource_read_failure_is_not_a_zero_measurement(self):
        with patch.object(perf_matrix.os, "listdir", side_effect=ProcessLookupError("gone")):
            sample = perf_matrix.sample_resources(123)
        self.assertFalse(sample.get("valid", True))
        self.assertTrue(sample.get("failureReasons"))

    def test_overhead_ratio(self):
        self.assertAlmostEqual(perf_matrix.ratio(80.0, 100.0), 0.8)


class FakeClock:
    def __init__(self):
        self.now = 0.0
        self.on_sleep = lambda: None

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.on_sleep()
        self.now += seconds


class FakeProcess:
    def __init__(self, clock, duration, output, returncode, pid, stdout):
        self.clock = clock
        self.started = clock.now
        self.duration = duration
        self.output = output
        self.exitcode = returncode
        self.returncode = None
        self.pid = pid
        if stdout not in (None, subprocess.PIPE):
            try:
                stdout.write(output.encode())
            except TypeError:
                stdout.write(output)
            stdout.flush()

    def poll(self):
        if self.clock.now >= self.started + self.duration:
            self.returncode = self.exitcode
        return self.returncode

    def communicate(self, timeout=None):
        self.wait(timeout)
        return self.output, None

    def wait(self, timeout=None):
        remaining = max(0, self.started + self.duration - self.clock.now)
        if self.returncode is None and timeout is not None and remaining > timeout:
            self.clock.sleep(timeout)
            raise subprocess.TimeoutExpired("load", timeout)
        if self.returncode is None:
            self.clock.sleep(remaining)
            self.returncode = self.exitcode
        return self.returncode

    def kill(self):
        self.returncode = -9
        self.exitcode = -9
        self.duration = 0

    terminate = kill


def client_output(protocol, errors=0, throughput=100):
    if protocol == "wrk":
        return "raw client output\nWRK_JSON " + json.dumps({
            "throughput": throughput, "p50": 10, "p95": 20, "p99": 30, "errors": errors}) + "\n"
    if protocol == "h2load":
        return (f"requests: 100 total, 100 started, 100 done, 100 succeeded, {errors} failed, 0 errored, 0 timeout\n"
                f"req/s : {throughput}\n1000\t200\t10\n1100\t200\t20\n")
    return "raw client output\n" + json.dumps({
        "messages": throughput, "elapsed_ms": 1000, "p50_us": 10,
        "p95_us": 20, "p99_us": 30, "errors": errors}) + "\n"


class PerfExecutionTest(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.out = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        self.clock = FakeClock()
        self.processes = []
        self.durations = []
        self.duration = 1800
        self.exitcode = 0
        self.output = client_output("json")
        self.missing_resources = False
        self.live_rows = []
        self.launch_rows = []
        self.args = SimpleNamespace(out=self.out, build=Path("build"), runs=1,
                                    duration=0, timeout=3600, server_pid=123, loop_count=1)
        self.plan = {"sampling": {"runs": 1, "measureSeconds": 30, "cooldownSeconds": 5}}
        self.case = {"name": "probe", "type": "soak", "protocol": "json", "url": "unused",
                     "command": "load {duration}", "durationSeconds": 1800, "concurrency": 2}
        self.stack.enter_context(patch.object(perf_matrix.time, "monotonic", self.clock.monotonic))
        self.stack.enter_context(patch.object(perf_matrix.time, "sleep", self.clock.sleep))
        self.stack.enter_context(patch.object(perf_matrix.subprocess, "Popen", self.launch))
        self.stack.enter_context(patch.object(perf_matrix.subprocess, "run", self.run_subprocess))
        self.stack.enter_context(patch.object(perf_matrix, "sample_resources", self.resources))
        self.clock.on_sleep = self.observe_jsonl

    def run_subprocess(self, command, **kwargs):
        if isinstance(command, list) and command[0] == "git":
            return subprocess.CompletedProcess(command, 0, "test-commit", "")
        self.clock.sleep(self.duration)
        return subprocess.CompletedProcess(command, self.exitcode, self.output, "")

    def rows(self):
        return [json.loads(line) for path in self.out.glob("*.jsonl")
                for line in path.read_text(encoding="utf-8").splitlines()]

    def observe_jsonl(self):
        if self.clock.now < 1800:
            self.live_rows = self.rows()

    def launch(self, command, **kwargs):
        self.launch_rows.append(self.rows())
        duration = self.durations.pop(0) if self.durations else self.duration
        process = FakeProcess(self.clock, duration, self.output, self.exitcode,
                              8000 + len(self.processes), kwargs.get("stdout"))
        self.processes.append(process)
        return process

    def resources(self, pid):
        if self.missing_resources and self.clock.now >= 900:
            return {"fds": 0, "rssBytes": 0, "valid": False, "failureReasons": ["process gone"]}
        return {"fds": 100 if not self.processes or self.clock.now >= 1805 else 300,
                "rssBytes": 1000 if not self.processes else 2000, "valid": True}

    def run_main(self):
        self.clock.now = 0
        self.processes.clear()
        self.plan["cases"] = {"probe": self.case}
        argv = ["perf_matrix", "--plan", __file__, "--case", "probe", "--out", str(self.out),
                "--server-pid", "123", "--runs", "1"]
        with patch.object(sys, "argv", argv), patch.object(perf_matrix.yaml, "safe_load", return_value=self.plan):
            try:
                status = perf_matrix.main()
            except Exception as error:
                self.fail(f"main must preserve failed-run evidence instead of raising: {error!r}")
        return status, json.loads((self.out / "probe.json").read_text(encoding="utf-8"))

    def test_soak_stable_with_live_evidence_and_parallel_clients(self):
        result = perf_matrix.run_soak(self.case, self.plan, self.args)
        self.assertTrue(result["stable"])
        self.assertGreaterEqual(result.get("seconds", 0), 1800)
        self.assertFalse(result["failureReasons"])
        self.assertEqual([process.started for process in self.processes], [0, 0])
        self.assertEqual(self.launch_rows[0][0]["phase"], "baseline")
        self.assertGreater(len(self.live_rows), 100)
        rows = self.rows()
        self.assertEqual(rows[-1]["phase"], "quiet")
        self.assertGreaterEqual(rows[-1]["time"], result["seconds"] + 5)
        times = [row["time"] for row in rows if row["phase"] == "load"]
        self.assertEqual(times, list(range(0, 1801, 10)))
        self.assertEqual(result["metrics"]["percentileScope"], "percentiles-of-client-percentiles")
        for client in result["metrics"]["clients"]:
            self.assertEqual(Path(client["outputPath"]).read_text(encoding="utf-8"), self.output)

    def test_soak_rejects_disappearing_server(self):
        self.missing_resources = True
        result = perf_matrix.run_soak(self.case, self.plan, self.args)
        self.assertFalse(result["stable"])
        self.assertTrue(result["failureReasons"])

    def test_soak_rejects_one_early_client_even_if_other_runs_full_duration(self):
        self.durations = [900, 1800]
        result = perf_matrix.run_soak(self.case, self.plan, self.args)
        self.assertFalse(result["stable"])
        self.assertIn("early-exit", " ".join(result["failureReasons"]))

    def test_soak_rejects_nonzero_exit_despite_valid_output(self):
        self.exitcode = 7
        result = perf_matrix.run_soak(self.case, self.plan, self.args)
        self.assertFalse(result["stable"])
        self.assertIn("nonzero-exit", " ".join(result["failureReasons"]))

    def test_short_soak_cannot_claim_thirty_minute_stability(self):
        self.case["durationSeconds"] = self.duration = 60
        result = perf_matrix.run_soak(self.case, self.plan, self.args)
        self.assertFalse(result["stable"])
        self.assertIn("insufficient-duration", " ".join(result["failureReasons"]))

    def test_main_errors_are_red_for_every_branch_and_protocol(self):
        for kind in ("capacity", "overhead", "soak"):
            for protocol in ("json", "wrk", "h2load"):
                with self.subTest(kind=kind, protocol=protocol):
                    self.output = client_output(protocol, errors=1)
                    self.duration = 1800 if kind == "soak" else 30
                    self.case.update(type=kind, protocol=protocol, concurrency=2 if kind == "soak" else [2])
                    self.case["variants"] = {"on": {}, "off": {}}
                    status, result = self.run_main()
                    self.assertEqual(status, 1)
                    self.assertEqual(result["verdict"], "red")
                    self.assertTrue(result["failureReasons"])

    def test_capacity_warms_up_and_cools_down_every_measurement(self):
        self.case.update(type="capacity", concurrency=[1])
        self.plan["sampling"].update(warmupSeconds=10, cooldownSeconds=5)
        self.args.runs = 2
        self.durations = [10, 30, 10, 30]
        result = perf_matrix.run_capacity(self.case, self.plan, self.args)
        self.assertFalse(result["failureReasons"])
        self.assertEqual([process.started for process in self.processes], [0, 10, 45, 55])
        self.assertEqual(self.clock.now, 90)
        self.assertEqual([sample["seconds"] for sample in result["points"][0]["runs"]], [30, 30])

    def test_main_valid_capacity_is_green(self):
        self.duration = 30
        self.case.update(type="capacity", concurrency=[2])
        status, result = self.run_main()
        self.assertEqual(status, 0)
        self.assertEqual(result.get("verdict"), "green")
        self.assertEqual(result["points"][0]["percentileScope"], "median-of-run-percentiles")

    def test_main_nonzero_exit_is_red_for_every_branch(self):
        self.exitcode = 2
        for kind in ("capacity", "overhead", "soak"):
            for protocol in ("json", "wrk", "h2load"):
                with self.subTest(kind=kind, protocol=protocol):
                    self.output = client_output(protocol)
                    self.duration = 1800 if kind == "soak" else 30
                    self.case.update(type=kind, protocol=protocol, concurrency=2 if kind == "soak" else [2])
                    self.case["variants"] = {"off": {}}
                    status, result = self.run_main()
                    self.assertEqual(status, 1)
                    self.assertIn("nonzero-exit", " ".join(result["failureReasons"]))

    def test_main_no_throughput_or_valid_output_is_red(self):
        for kind in ("capacity", "overhead", "soak"):
            for protocol in ("json", "wrk", "h2load"):
                for output in (client_output(protocol, throughput=0), "unparseable raw output\n"):
                    with self.subTest(kind=kind, protocol=protocol, output=output):
                        self.duration = 1800 if kind == "soak" else 30
                        self.case.update(type=kind, protocol=protocol, concurrency=1 if kind == "soak" else [1])
                        self.case["variants"] = {"off": {}}
                        self.output = output
                        status, result = self.run_main()
                        self.assertEqual(status, 1)
                        self.assertTrue(result["failureReasons"])
                        if kind == "soak":
                            metrics = result["metrics"]
                        elif kind == "overhead":
                            metrics = result["variants"]["off"]["points"][0]["runs"][0]
                        else:
                            metrics = result["points"][0]["runs"][0]
                        path = Path(metrics["clients"][0]["outputPath"])
                        self.assertEqual(path.read_text(encoding="utf-8"), output)

    def test_timeout_still_reclaims_group_if_shell_exits_during_cleanup(self):
        self.case.update(type="capacity", concurrency=[1])
        self.args.timeout = 2
        killed = []
        launch = self.launch

        def exiting_shell(command, **kwargs):
            process = launch(command, **kwargs)
            polls = []

            def poll():
                if self.clock.now >= 2:
                    polls.append(1)
                    if len(polls) > 1:
                        process.returncode = 0
                return process.returncode

            process.poll = poll
            return process

        def killpg(pid, sig):
            killed.append(pid)
            self.processes[0].kill()

        with (patch.object(perf_matrix.subprocess, "Popen", exiting_shell),
              patch.object(perf_matrix.os, "name", "posix"),
              patch.object(perf_matrix.os, "killpg", killpg, create=True),
              patch.object(perf_matrix.signal, "SIGKILL", 9, create=True)):
            result = perf_matrix.run_capacity(self.case, self.plan, self.args)
        self.assertIn("timeout", " ".join(result["failureReasons"]))
        self.assertEqual(killed, [8000])

    def test_timeout_reclaims_only_created_linux_groups_and_keeps_output(self):
        self.case.update(type="capacity", concurrency=[2])
        self.args.timeout = 2
        self.duration = 10000
        killed = []

        def killpg(pid, sig):
            killed.append(pid)
            next(process for process in self.processes if process.pid == pid).kill()

        with (patch.object(perf_matrix.os, "name", "posix"),
              patch.object(perf_matrix.os, "killpg", killpg, create=True),
              patch.object(perf_matrix.signal, "SIGKILL", 9, create=True)):
            try:
                result = perf_matrix.run_capacity(self.case, self.plan, self.args)
            except subprocess.TimeoutExpired:
                self.fail("timeout must reclaim client groups and return red evidence")
        self.assertEqual(set(killed), {8000, 8001})
        self.assertTrue(all(process.poll() is not None for process in self.processes))
        self.assertIn("timeout", " ".join(result["failureReasons"]))
        self.assertLess(self.clock.now, 20)
        for client in result["points"][0]["runs"][0]["clients"]:
            self.assertEqual(Path(client["outputPath"]).read_text(encoding="utf-8"), self.output)


class PerfRealProcessTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="perf matrix ")
        self.addCleanup(self.directory.cleanup)
        self.args = SimpleNamespace(out=Path(self.directory.name), build=Path("build"), runs=1,
                                    duration=0, timeout=10, server_pid=0, loop_count=1)
        self.plan = {"sampling": {"runs": 1, "measureSeconds": 30}}
        self.case = {"name": "real", "protocol": "json", "url": "unused", "concurrency": [1]}

    def command(self, code):
        argv = [sys.executable, "-B", "-c", code]
        return shlex.join(argv) if os.name == "posix" else subprocess.list2cmdline(argv)

    def test_real_process_exit_and_raw_stdout_stderr(self):
        for code in (0, 7):
            with self.subTest(exitcode=code):
                self.case["command"] = self.command(
                    f"import sys; print({client_output('json')!r}, end=''); "
                    f"print('stderr evidence', file=sys.stderr); sys.exit({code})")
                result = perf_matrix.run_capacity(self.case, self.plan, self.args)
                self.assertEqual(bool(result["failureReasons"]), code != 0)
                client = result["points"][0]["runs"][0]["clients"][0]
                self.assertEqual(client["returncode"], code)
                raw = Path(client["outputPath"]).read_text(encoding="utf-8")
                self.assertIn("stderr evidence", raw)
                self.assertIn(client_output("json"), raw)

    @unittest.skipUnless(os.name == "posix" and Path("/proc").is_dir(), "Linux process-group integration")
    def test_timeout_kills_descendants_without_touching_other_group(self):
        other = subprocess.Popen([sys.executable, "-B", "-c", "import time; time.sleep(60)"], start_new_session=True)
        self.addCleanup(other.wait)
        self.addCleanup(other.kill)
        self.args.timeout = 1
        self.case["command"] = self.command(
            "import subprocess, sys, time; "
            "child = subprocess.Popen([sys.executable, '-B', '-c', 'import time; time.sleep(60)']); "
            "print(child.pid, flush=True); time.sleep(60)")
        result = perf_matrix.run_capacity(self.case, self.plan, self.args)
        self.assertIn("timeout", " ".join(result["failureReasons"]))
        self.assertIsNone(other.poll())
        client = result["points"][0]["runs"][0]["clients"][0]
        child = int(Path(client["outputPath"]).read_text(encoding="utf-8").strip())
        status = Path(f"/proc/{child}/stat")
        if status.exists():
            self.assertEqual(status.read_text().split()[2], "Z")


if __name__ == "__main__":
    unittest.main()
