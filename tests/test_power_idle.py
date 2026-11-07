"""Compile the production idle refresher against a native-counter model."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc

ROOT = Path(__file__).resolve().parents[1]


class PowerIdleTests(unittest.TestCase):
    def test_native_counter_contract(self):
        gcc = native_gcc()
        if not Path(gcc).is_file():
            self.skipTest("GCC is required for the production C model")
        env = dict(os.environ)
        env["PATH"] = str(Path(gcc).parent) + os.pathsep + env.get("PATH", "")
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "power-idle.exe"
            subprocess.run([gcc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tests/power_idle_model.c"), "-o", str(exe)],
                           check=True, capture_output=True, text=True, env=env)
            result = subprocess.run([str(exe)], check=True, capture_output=True,
                                    text=True, env=env, timeout=15)
            self.assertIn("PASS", result.stdout)

    def test_fingerprint_matches_captured_firmware(self):
        import struct
        path = ROOT / "reference/hardware-kernel-body.bin"
        if not path.is_file():
            self.skipTest("private captured firmware unavailable")
        body = path.read_bytes()
        base = 0x02000000
        self.assertEqual(struct.unpack_from("<I", body, 0x02000204 - base)[0], 0x021E7B54)
        self.assertEqual(struct.unpack_from("<I", body, 0x02000200 - base)[0], 0x021E81E8)
        self.assertEqual(struct.unpack_from("<I", body, 0x021E81E8 + 23*4 - base)[0], 0x021CE8AE)
        self.assertEqual(struct.unpack_from("<I", body, 0x021E81E8 + 24*4 - base)[0], 0x021CE8D8)
        self.assertEqual(struct.unpack_from("<I", body, 0x021E7B54 + 0x64C - base)[0], 0x02198C94)
        self.assertEqual(body[0x02198C94 - base:0x02198C94 - base + 10],
                         bytes.fromhex("046c77d254cff43c4006"))


if __name__ == "__main__":
    unittest.main()
