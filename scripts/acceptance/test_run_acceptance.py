import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_acceptance


def make_row(root: Path, identifier: str, command: str):
    script = root / "scripts" / "probe.sh"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    return {
        "id": identifier,
        "module": "Net",
        "capability": "探针",
        "claim_source": "README.md",
        "platform": "linux",
        "runner": "ecs",
        "prerequisites": [],
        "entry": "script:scripts/probe.sh",
        "command": command,
        "oracle": "退出 0",
        "evidence": f"runs/{identifier}.log",
        "status": "unverified",
        "defect": "",
        "regression_test": "",
    }


class RunnerTest(unittest.TestCase):
    def test_render_replaces_server_pid(self):
        args = SimpleNamespace(repo_root=Path("/repo"), evidence_root=Path("/runs"), build=Path("/build"), python="python", server_pid=4242)
        command = run_acceptance.render("sample --pid {server_pid}", args, {"id": "PERF.SOAK.H1"})
        self.assertIn("4242", command)

    def test_pass_command_becomes_green_and_writes_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("", encoding="utf-8")
            row = make_row(root, "NET.PROBE", "{python} -c \"print('ok')\"")
            args = SimpleNamespace(repo_root=root, evidence_root=root / "runs", build=root / "build", python=sys.executable, timeout=10.0)
            results = run_acceptance.run_rows([row], args)
            self.assertEqual(results[0]["status"], "green")
            self.assertTrue((root / "runs" / "NET.PROBE.log").is_file())

    def test_fail_command_becomes_red(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("", encoding="utf-8")
            row = make_row(root, "NET.PROBE", "{python} -c \"raise SystemExit(3)\"")
            args = SimpleNamespace(repo_root=root, evidence_root=root / "runs", build=root / "build", python=sys.executable, timeout=10.0)
            results = run_acceptance.run_rows([row], args)
            self.assertEqual(results[0]["status"], "red")
            self.assertIn("3", results[0]["reason"])

    def test_no_skip_oracle_rejects_successful_process_with_skips(self):
        for output in ("[  SKIPPED ] Probe.Case", "1/1 Test #1: Probe.Case ...***Skipped"):
            with self.subTest(output=output), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                row = make_row(root, "NET.PROBE", "{python} -c \"print('" + output + "')\"")
                row["oracle"] = "过滤集全通过且无 SKIP"
                args = SimpleNamespace(repo_root=root, evidence_root=root / "runs", build=root / "build", python=sys.executable, timeout=10.0)
                results = run_acceptance.run_rows([row], args)
                self.assertEqual(results[0]["status"], "red")
                self.assertIn("SKIP", results[0]["reason"])

    def test_evidence_paths_are_written_relative_to_the_evidence_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("", encoding="utf-8")
            row = make_row(root, "NET.PROBE", "{python} -c \"print('ok')\"")
            row["evidence"] = "custom/PROBE.log"
            args = SimpleNamespace(repo_root=root, evidence_root=root / "runs", build=root / "build", python=sys.executable, timeout=10.0, dry_run=False)
            run_acceptance.run_rows([row], args)
            self.assertTrue((root / "runs/custom/PROBE.log").is_file())

    def test_posix_environment_assignment_runs_on_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("", encoding="utf-8")
            command = (
                "ASYN_PROBE_VALUE=42 {python} -c "
                "\"import os; raise SystemExit(0 if os.environ.get('ASYN_PROBE_VALUE') == '42' else 1)\""
            )
            row = make_row(root, "NET.PROBE", command)
            args = SimpleNamespace(repo_root=root, evidence_root=root / "runs", build=root / "build", python=sys.executable, timeout=10.0)
            results = run_acceptance.run_rows([row], args)
            self.assertEqual(results[0]["status"], "green")

    def test_dry_run_prints_without_running(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("", encoding="utf-8")
            row = make_row(root, "NET.PROBE", "{python} -c \"print('ok')\"")
            args = SimpleNamespace(repo_root=root, evidence_root=root / "runs", build=root / "build", python=sys.executable, timeout=10.0, dry_run=True)
            results = run_acceptance.run_rows([row], args)
            self.assertEqual(results[0]["status"], "dry-run")
            self.assertFalse((root / "runs" / "NET.PROBE.log").exists())


class PerformanceDriverTest(unittest.TestCase):
    def test_all_servers_receive_continuous_load_configuration(self):
        import shlex
        import shutil
        import subprocess
        import yaml
        driver = (Path(__file__).parent / "waves/w8_perf.sh").read_text(encoding="utf-8")
        setup = driver[driver.index('mkdir -p "$evidence/servers"'):driver.index('pids=()')]
        function = driver.split("start_server() {", 1)[1].split("\n}\n", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            command = (
                f"evidence={shlex.quote(root.as_posix())}\n" + setup
                + '\nrecord_reference() { printf "%s\\n" "$@" >> "$evidence/arguments"; }\nreference=record_reference\n'
                + "start_server() {" + function + "\n}\n"
                + "start_server h2c --h2c\nwait\nstart_server tracing-on\nwait\n"
            )
            completed = subprocess.run([shutil.which("bash"), "-c", command], capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(completed.returncode, 0, completed.stderr)
            arguments = (root / "arguments").read_text().splitlines()
            self.assertEqual(arguments.count("--config"), 2)
            for name in ("server.yaml", "tracing.yaml"):
                configuration = yaml.safe_load((root / name).read_text())
                self.assertFalse(configuration["server"]["access_log"])
                self.assertEqual(configuration["server"]["limits"]["maximum_requests_per_connection"], 0)
                self.assertIn((root / name).as_posix(), arguments)

    def test_failed_wave_is_preserved_and_reported(self):
        import shlex
        import shutil
        import subprocess
        driver = (Path(__file__).parent / "waves/w8_perf.sh").read_text(encoding="utf-8")
        function = driver.split("run_rows() {", 1)[1].split("\n}\n", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "scripts/acceptance/run_acceptance.py"
            script.parent.mkdir(parents=True)
            script.write_text(
                "import pathlib, sys\n"
                "evidence = pathlib.Path(sys.argv[sys.argv.index('--evidence-root') + 1])\n"
                "evidence.mkdir(parents=True, exist_ok=True)\n"
                "(evidence / 'results-all.json').write_text('[{\"status\":\"red\"}]')\n"
                "raise SystemExit(7)\n", encoding="utf-8")
            quoted_root = shlex.quote(root.as_posix())
            command = (
                f"repo={quoted_root}; evidence={quoted_root}; build=build; "
                f"python={shlex.quote(Path(sys.executable).as_posix())}; overall_status=0; "
                "run_rows() {" + function + "\n}\n"
                'run_rows PERF.SOAK.H1\nexit "$overall_status"\n'
            )
            completed = subprocess.run([shutil.which("bash"), "-c", command], capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(completed.returncode, 7, completed.stderr)
            self.assertEqual((root / "results-PERF_SOAK_H1.json").read_text(), '[{"status":"red"}]')


if __name__ == "__main__":
    unittest.main()
