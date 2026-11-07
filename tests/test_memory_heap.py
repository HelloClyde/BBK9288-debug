"""Execute the production independent-download allocator wrapper offline."""
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc, target_compiler

ROOT = Path(__file__).resolve().parents[1]
SIGNATURES = {
    "malloc": (0x021D330C, 260), "free": (0x021D353A, 330),
    "init": (0x021D3410, 58), "shift_down": (0x021D3B08, 18),
    "shift_up": (0x021D3B1A, 28), "lock": (0x021CE8AE, 42),
    "unlock": (0x021CE8D8, 58),
}


class MemoryHeapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = native_gcc()
        if not Path(gcc).is_file():
            raise unittest.SkipTest("GCC required for production C model")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.temporary.name) / "memory-heap-model.exe"
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(gcc).parent) + os.pathsep + cls.env.get("PATH", "")
        subprocess.run([gcc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/memory_heap_model.c"), "-o", str(cls.exe)],
                       check=True, capture_output=True, text=True, env=cls.env)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.exe), str(case)], check=True,
                                capture_output=True, text=True, env=self.env, timeout=15)
        self.assertIn("PASS", result.stdout)

    def test_independent_live_65_record_stats_and_crc(self):
        self.run_case(0)

    def test_maximum_probe_errno_restore_failure_and_unexpected_boundary_cleanup(self):
        self.run_case(1)

    def test_benchmark_owner_reserve_alignment_bounds_and_release(self):
        self.run_case(2)

    def test_directory_layout_overflow_fragmentation_and_record_limit(self):
        self.run_case(3)

    def test_complete_code_roots_ordinary_context_and_lock_races(self):
        self.run_case(4)

    def test_all_native_code_bytes_match_private_capture(self):
        path = ROOT / "reference/hardware-kernel-body.bin"
        if not path.is_file():
            self.skipTest("private captured kernel unavailable")
        body = path.read_bytes()
        source = (ROOT / "memory/memory_heap.c").read_text(encoding="utf-8-sig")
        for name, (address, size) in SIGNATURES.items():
            match = re.search(rf"heap_{name}_signature\[{size}\]=\{{(.*?)\}};", source, re.S)
            self.assertIsNotNone(match, name)
            data = bytes(int(item, 16) for item in re.findall(r"0x([0-9a-fA-F]{2})", match[1]))
            self.assertEqual(len(data), size, name)
            self.assertEqual(data, body[address-0x02000000:address-0x02000000+size], name)
        for address, value in ((0x02000200, 0x021E81E8), (0x02000210, 0x0200021C),
                               (0x0200021C, 0x021D330C), (0x02000220, 0x021D353A),
                               (0x0200022C, 0x021D3410),
                               (0x021E81E8+23*4, 0x021CE8AE),
                               (0x021E81E8+24*4, 0x021CE8D8)):
            self.assertEqual(struct.unpack_from("<I", body, address-0x02000000)[0], value)

    def test_strict_s1c33_compilation(self):
        clang = target_compiler()
        if not clang.is_file():
            self.skipTest("S1C33 target toolchain unavailable")
        obj = Path(self.temporary.name) / "memory_heap.o"
        subprocess.run([str(clang), "--target=s1c33-none-elf", "-Os", "-ffreestanding",
                        "-fno-builtin", "-Wall", "-Wextra", "-Werror", "-c",
                        str(ROOT / "memory/memory_heap.c"), "-o", str(obj)],
                       check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
