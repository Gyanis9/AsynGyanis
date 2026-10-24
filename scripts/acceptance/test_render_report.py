import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import render_report


class ReportTest(unittest.TestCase):
    def test_table_contains_status_and_evidence(self):
        rows = [{
            "id": "NET.HTTP1.MESSAGE",
            "capability": "HTTP/1.1",
            "runner": "ecs",
            "status": "green",
            "evidence": "runs/w2/http1.log",
        }]
        text = render_report.capability_table(rows)
        self.assertIn("NET.HTTP1.MESSAGE", text)
        self.assertIn("green", text)
        self.assertIn("runs/w2/http1.log", text)

    def test_na_rows_are_listed_separately(self):
        rows = [{
            "id": "NET.QUIC.PROXY",
            "capability": "QUIC PROXY",
            "runner": "ecs",
            "status": "n-a",
            "defect": "未声称；补验条件：提供 QUIC 代理",
            "evidence": "",
        }]
        text = render_report.na_table(rows)
        self.assertIn("补验条件", text)
        self.assertIn("NET.QUIC.PROXY", text)

    def test_verdict_counts(self):
        rows = [{"status": "green"}, {"status": "green"}, {"status": "n-a"}]
        counts = render_report.verdict_counts(rows)
        self.assertEqual(counts["green"], 2)
        self.assertEqual(counts["n-a"], 1)


if __name__ == "__main__":
    unittest.main()
