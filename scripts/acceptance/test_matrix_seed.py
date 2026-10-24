import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest

REPO = Path(__file__).resolve().parents[2]
MATRIX = REPO / "tests" / "acceptance" / "capabilities.yaml"
WAVES = REPO / "tests" / "acceptance" / "waves.yaml"


class SeedMatrixTest(unittest.TestCase):
    def test_matrix_is_valid(self):
        rows = manifest.load_manifest(MATRIX)
        self.assertEqual(manifest.validate_manifest(rows, REPO), [])

    def test_required_domains_are_present(self):
        rows = manifest.load_manifest(MATRIX)
        ids = {row["id"] for row in rows}
        for required in (
            "PLATFORM.EVENT.BACKENDS",
            "PLATFORM.EVENT.URING",
            "PLATFORM.WINDOWS.SMOKE",
            "CORE.ASAN",
            "CORE.TSAN",
            "NET.FUZZ",
            "DB.MYSQL.TLS",
            "CORE.COROUTINE",
            "CORE.TLS",
            "NET.HTTP1.MESSAGE",
            "NET.HTTP2",
            "NET.HTTP3",
            "NET.QUIC.TRANSPORT",
            "NET.WS",
            "NET.CLIENT",
            "NET.ACME",
            "NET.OBS.METRICS",
            "DB.SQLITE",
            "DB.MYSQL",
            "DEPLOY.REFERENCE.H1",
            "DEPLOY.MULTIPROCESS",
            "PERF.H1.CAPACITY",
            "PERF.H2TLS.CAPACITY",
            "PERF.SOAK.H1",
            "PERF.SOAK.CHURN",
            "PERF.OVERHEAD.TLS",
        ):
            self.assertIn(required, ids)

    def test_every_row_belongs_to_exactly_one_wave(self):
        import yaml
        rows = manifest.load_manifest(MATRIX)
        waves = yaml.safe_load(WAVES.read_text(encoding="utf-8"))
        prefixes = [(wave, prefix) for wave, items in waves.items() for prefix in items]
        for row in rows:
            matched = [wave for wave, prefix in prefixes if row["id"].startswith(prefix)]
            self.assertEqual(len(matched), 1, f"{row['id']} 命中波次 {matched}")


class PerformancePlanTest(unittest.TestCase):
    def setUp(self):
        import yaml
        self.plan = yaml.safe_load((REPO / "tests/acceptance/perf-plan.yaml").read_text(encoding="utf-8"))

    def test_http2_commands_use_the_configured_duration(self):
        import shlex
        for name in ("h2", "h2tls", "soak_h2"):
            with self.subTest(case=name):
                tokens = shlex.split(self.plan["cases"][name]["command"])
                self.assertIn("-D", tokens)
                self.assertEqual(tokens[tokens.index("-D") + 1], "{duration}s")
                self.assertNotIn("-n", tokens)

    def test_churn_uses_timed_load_and_shared_resource_verdict(self):
        import shlex
        rows = manifest.load_manifest(MATRIX)
        row = next(row for row in rows if row["id"] == "PERF.SOAK.CHURN")
        self.assertIn("--case soak_churn", row["command"])
        self.assertIn("--server-pid {server_pid}", row["command"])
        case = self.plan["cases"]["soak_churn"]
        self.assertEqual(case["type"], "soak")
        self.assertEqual(case["protocol"], "wrk")
        self.assertGreaterEqual(case["durationSeconds"], 1800)
        tokens = shlex.split(case["command"])
        self.assertIn("-d{duration}s", tokens)
        self.assertEqual(tokens[tokens.index("-H") + 1], "Connection: close")


if __name__ == "__main__":
    unittest.main()
