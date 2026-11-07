"""Production desktop GUI bridge model; no physical device access."""
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
    "post": (0x02182C3A, 144), "lookup": (0x0218231A, 64),
    "remainder": (0x021D5120, 38), "lock": (0x021CE8AE, 42),
    "unlock": (0x021CE8D8, 58), "desktop_hook": (0x021813C2, 28),
    "gui_entry": (0x0218339A, 24),
    "desktop_default": (0x021813DE, 214), "desktop_return": (0x02181814, 8),
}


class GuiJobsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = native_gcc()
        if not Path(gcc).is_file():
            raise unittest.SkipTest("GCC required for production C model")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.temporary.name) / "gui-jobs-model.exe"
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(gcc).parent) + os.pathsep + cls.env.get("PATH", "")
        subprocess.run([gcc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/gui_jobs_model.c"), "-o", str(cls.exe)],
                       check=True, capture_output=True, text=True, env=cls.env)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.exe), str(case)], check=True,
                                capture_output=True, text=True, env=self.env, timeout=15)
        self.assertIn("PASS", result.stdout)

    def test_one_step_desktop_lifecycle_and_coalescing(self):
        self.run_case(0)

    def test_foreign_hook_and_stop_ownership(self):
        self.run_case(1)

    def test_bounded_desktop_ring_full_retry_and_post_failure(self):
        self.run_case(2)

    def test_non_gui_synchronous_hook_irq_locks_psr_gp_and_message_validation(self):
        self.run_case(3)

    def test_reentry_only_worker_posts_next_step(self):
        self.run_case(4)

    def test_stop_restart_preserves_nonce_and_refuses_wrap(self):
        self.run_case(5)

    def test_complete_fingerprints_roots_prelock_race_and_lock_failure(self):
        self.run_case(6)

    def test_launcher_inline_stop_drains_without_gui_pump_or_ring_space(self):
        self.run_case(7)

    def test_native_signatures_and_table_mapping_match_capture(self):
        path = ROOT / "reference/hardware-kernel-body.bin"
        if not path.is_file():
            self.skipTest("private captured kernel unavailable")
        body = path.read_bytes()
        source = (ROOT / "device/gui_jobs.c").read_text(encoding="utf-8-sig")
        for name, (address, size) in SIGNATURES.items():
            match = re.search(rf"{name}_signature\[{size}\]=\{{(.*?)\}};", source, re.S)
            self.assertIsNotNone(match, name)
            data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", match[1]))
            self.assertEqual(len(data), size, name)
            self.assertEqual(data, body[address-0x02000000:address-0x02000000+size], name)
        for address, value in ((0x02000204, 0x021E7B54), (0x021E7B90, 0x02182C3A),
                               (0x02000200, 0x021E81E8), (0x021E8244, 0x021CE8AE),
                               (0x021E8248, 0x021CE8D8), (0x022A9820, 0x02068B34)):
            self.assertEqual(struct.unpack_from("<I", body, address-0x02000000)[0], value)

    def test_strict_s1c33_compilation(self):
        clang = target_compiler()
        if not clang.is_file():
            self.skipTest("S1C33 toolchain unavailable")
        subprocess.run([str(clang), "--target=s1c33-none-elf", "-Os", "-ffreestanding",
                        "-fno-builtin", "-Wall", "-Wextra", "-Werror", "-c",
                        str(ROOT / "device/gui_jobs.c"), "-o",
                        str(Path(self.temporary.name) / "gui_jobs.o")],
                       check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
