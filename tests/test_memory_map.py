"""Compile the real read-only memory inventory against an independent model."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc

ROOT = Path(__file__).resolve().parents[1]


class MemoryMapTests(unittest.TestCase):
    def test_production_read_only_contract(self):
        gcc = native_gcc()
        if not Path(gcc).is_file():
            self.skipTest("GCC is required for the production C model")
        env = dict(os.environ)
        env["PATH"] = str(Path(gcc).parent) + os.pathsep + env.get("PATH", "")
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "memory-map.exe"
            subprocess.run([gcc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tests/memory_map_model.c"), "-o", str(exe)],
                           check=True, capture_output=True, text=True, env=env)
            result = subprocess.run([str(exe)], check=True, capture_output=True,
                                    text=True, env=env, timeout=15)
            self.assertIn("Memory map: PASS", result.stdout)

    def test_profile_matches_captured_firmware(self):
        import re
        import struct
        path = ROOT / "reference/hardware-kernel-body.bin"
        if not path.is_file():
            self.skipTest("private captured firmware unavailable")
        body = path.read_bytes()
        base = 0x02000000
        roots = [0x021e81e8, 0x021e7b54, 0x021e7ae0,
                 0x021e79d0, 0x0200021c, 0x021e7a3c]
        self.assertEqual(list(struct.unpack_from("<6I", body, 0x200)), roots)
        # All three existing fingerprints remain bound to the real image.
        source = (ROOT / "device/profiles.h").read_text()
        captured = source.split('2u, "hardware-capture-0215ebc4"', 1)[1]
        signatures = re.findall(r"\{((?:0x[0-9a-f]{2},?\s*){40})\}", captured)
        self.assertEqual(len(signatures), 3)
        for address, signature in zip((0x021ced06, 0x021d330c, 0x0219c1f2), signatures):
            expected = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", signature))
            self.assertEqual(body[address-base:address-base+40], expected)


if __name__ == "__main__":
    unittest.main()
