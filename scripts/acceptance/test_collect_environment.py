import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import collect_environment


class EnvironmentTest(unittest.TestCase):
    def test_parse_os_release(self):
        text = 'NAME="Ubuntu"\nVERSION_ID="24.04"\n'
        facts = collect_environment.parse_os_release(text)
        self.assertEqual(facts["NAME"], "Ubuntu")
        self.assertEqual(facts["VERSION_ID"], "24.04")

    def test_parse_meminfo_converts_to_bytes(self):
        text = "MemTotal:        2048000 kB\nMemAvailable:     512000 kB\n"
        facts = collect_environment.parse_meminfo(text)
        self.assertEqual(facts["MemTotalBytes"], 2048000 * 1024)
        self.assertEqual(facts["MemAvailableBytes"], 512000 * 1024)

    def test_checksum_detects_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "file.bin"
            path.write_bytes(b"abc")
            self.assertTrue(collect_environment.verify_sha256(path, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
            self.assertFalse(collect_environment.verify_sha256(path, "0" * 64))

    def test_ldd_output_finds_missing_libraries(self):
        output = "\tlinux-vdso.so.1 (0x00007ffd)\n\tlibssl.so.3 => not found\n\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f)\n"
        missing = collect_environment.missing_libraries(output)
        self.assertEqual(missing, ["libssl.so.3"])


if __name__ == "__main__":
    unittest.main()
