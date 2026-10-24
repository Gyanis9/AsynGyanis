import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import package_release


class PackageTest(unittest.TestCase):
    def test_collect_shared_libraries_from_ldd(self):
        output = "\tlibssl.so.3 => /usr/lib/x86_64-linux-gnu/libssl.so.3 (0x1)\n\tlibc.so.6 => /lib/libc.so.6 (0x2)\n"
        self.assertEqual(package_release.libraries_of(output), ["/usr/lib/x86_64-linux-gnu/libssl.so.3", "/lib/libc.so.6"])

    def test_checksums_written_for_every_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.txt").write_text("a", encoding="utf-8")
            (root / "b.txt").write_text("b", encoding="utf-8")
            package_release.write_checksums(root)
            text = (root / "SHA256SUMS").read_text(encoding="utf-8")
            self.assertIn("a.txt", text)
            self.assertIn("b.txt", text)

    def test_bundle_manifest_has_commit_and_flags(self):
        manifest = package_release.bundle_manifest("abc123", "Release", ["-O3"], ["samples/ReferenceServer"])
        self.assertEqual(manifest["commit"], "abc123")
        self.assertEqual(manifest["buildType"], "Release")
        self.assertIn("samples/ReferenceServer", manifest["files"])

    def test_ctest_metadata_patterns_cover_tests_and_include(self):
        self.assertIn("*_include.cmake", package_release.CTEST_METADATA_PATTERNS)
        self.assertIn("*_tests.cmake", package_release.CTEST_METADATA_PATTERNS)

    def test_system_libraries_are_not_bundled(self):
        self.assertFalse(package_release.should_bundle_library("/lib/x86_64-linux-gnu/libc.so.6"))
        self.assertFalse(package_release.should_bundle_library("/usr/lib/x86_64-linux-gnu/libstdc++.so.6"))
        self.assertTrue(package_release.should_bundle_library("/root/.conan2/p/b/openssl/lib/libssl.so.3"))

    def test_runtime_files_are_executables_and_extensionless_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "ReferenceServer"
            binary.write_text("", encoding="utf-8")
            windows_binary = root / "tool.exe"
            windows_binary.write_text("", encoding="utf-8")
            text = root / "README.md"
            text.write_text("", encoding="utf-8")
            self.assertTrue(package_release.is_runtime_file(binary))
            self.assertTrue(package_release.is_runtime_file(windows_binary))
            self.assertFalse(package_release.is_runtime_file(text))


if __name__ == "__main__":
    unittest.main()
