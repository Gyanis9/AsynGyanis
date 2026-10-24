import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest


def row(**overrides):
    base = {
        "id": "NET.HTTP1.MESSAGE",
        "module": "Net",
        "capability": "HTTP/1.1 消息解析",
        "claim_source": "docs/网络编程.md",
        "platform": "linux",
        "runner": "ecs",
        "prerequisites": [],
        "entry": "test:Http1",
        "command": "ctest --test-dir {build} -L Net -R Http1",
        "oracle": "全用例通过",
        "evidence": "runs/w2/http1.log",
        "status": "unverified",
        "defect": "",
        "regression_test": "",
    }
    base.update(overrides)
    return base


class ManifestValidationTest(unittest.TestCase):
    def test_valid_row_has_no_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "网络编程.md").write_text("", encoding="utf-8")
            self.assertEqual(manifest.validate_manifest([row()], root), [])

    def test_id_allows_underscore_segments(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "网络编程.md").write_text("", encoding="utf-8")
            self.assertEqual(manifest.validate_manifest([row(id="NET.WS.DEFLATE_HUB")], root), [])

    def test_duplicate_id_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "网络编程.md").write_text("", encoding="utf-8")
            errors = manifest.validate_manifest([row(), row()], root)
            self.assertTrue(any("重复" in error for error in errors))

    def test_missing_claim_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            errors = manifest.validate_manifest([row(claim_source="docs/missing.md")], Path(directory))
            self.assertTrue(any("claim_source" in error for error in errors))

    def test_green_requires_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "网络编程.md").write_text("", encoding="utf-8")
            errors = manifest.validate_manifest([row(status="green", evidence="")], root)
            self.assertTrue(any("green" in error and "evidence" in error for error in errors))

    def test_na_requires_reason_and_retest_condition(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "网络编程.md").write_text("", encoding="utf-8")
            errors = manifest.validate_manifest([row(status="n-a", defect="")], root)
            self.assertTrue(any("n-a" in error and "defect" in error for error in errors))

    def test_script_entry_must_exist(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "网络编程.md").write_text("", encoding="utf-8")
            errors = manifest.validate_manifest([row(entry="script:scripts/missing.sh")], root)
            self.assertTrue(any("script" in error for error in errors))

    def test_manifest_round_trip(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capabilities.yaml"
            manifest.write_manifest(path, [row()])
            loaded = manifest.load_manifest(path)
            self.assertEqual(loaded[0]["id"], row()["id"])

    def test_manifest_writes_one_row_per_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capabilities.yaml"
            manifest.write_manifest(path, [row(), row(id="NET.SECOND")])
            self.assertEqual(len(path.read_text(encoding="utf-8").splitlines()), 2)


if __name__ == "__main__":
    unittest.main()
