import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import otlp_collector


class CollectorTest(unittest.TestCase):
    def test_known_paths_are_accepted(self):
        self.assertTrue(otlp_collector.is_otlp_path("/v1/metrics"))
        self.assertTrue(otlp_collector.is_otlp_path("/v1/logs"))
        self.assertTrue(otlp_collector.is_otlp_path("/v1/traces"))
        self.assertFalse(otlp_collector.is_otlp_path("/healthz"))

    def test_record_shape_contains_path_and_size(self):
        record = otlp_collector.make_record("/v1/logs", b"{}")
        self.assertEqual(record["path"], "/v1/logs")
        self.assertEqual(record["bytes"], 2)


if __name__ == "__main__":
    unittest.main()
