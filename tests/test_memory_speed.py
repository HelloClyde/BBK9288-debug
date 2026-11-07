"""Run fixed memory kernels with owned buffers and a modeled device clock."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc

ROOT = Path(__file__).resolve().parents[1]


class MemorySpeedTests(unittest.TestCase):
    def test_device_elapsed_and_memory_contract(self):
        gcc = native_gcc()
        if not Path(gcc).is_file():
            self.skipTest("GCC is required for the production C model")
        env = dict(os.environ)
        env["PATH"] = str(Path(gcc).parent) + os.pathsep + env.get("PATH", "")
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "memory-speed.exe"
            subprocess.run([gcc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tests/memory_speed_model.c"), "-o", str(exe)],
                           check=True, capture_output=True, text=True, env=env)
            result = subprocess.run([str(exe)], check=True, capture_output=True,
                                    text=True, env=env, timeout=25)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
