"""Production native refresh under an owned GUI1 model; no physical device."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc, target_compiler

ROOT = Path(__file__).resolve().parents[1]


class AppRefreshTests(unittest.TestCase):
    def test_native_scanner_guards_and_partial_results(self):
        compiler = native_gcc()
        if not compiler or not Path(compiler).is_file():
            self.skipTest("GCC required")
        env = dict(os.environ)
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "refresh-model.exe"
            subprocess.run([
                compiler, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                str(ROOT / "tests/app_refresh_model.c"), "-o", str(executable),
            ], check=True, capture_output=True, text=True, env=env)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, env=env, timeout=20)
        self.assertIn("PASS native app scanner", result.stdout)

    def test_actual_s1c33_compile_uses_only_two_owned_external_guards(self):
        compiler = target_compiler()
        if not compiler.is_file():
            self.skipTest("S1C33 compiler required")
        from elftools.elf.elffile import ELFFile
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "app-refresh.o"
            subprocess.run([
                str(compiler), "--target=s1c33-none-elf", "-Os", "-ffreestanding",
                "-fno-builtin", "-fno-jump-tables", "-fomit-frame-pointer",
                "-Wall", "-Wextra", "-Werror", "-c", str(ROOT / "device/app_refresh.c"),
                "-o", str(output),
            ], check=True, capture_output=True, text=True)
            with output.open("rb") as stream:
                elf = ELFFile(stream)
                undefined = {symbol.name for symbol in elf.get_section_by_name(".symtab").iter_symbols()
                             if symbol.name and symbol["st_shndx"] == "SHN_UNDEF"}
        self.assertEqual(undefined, {"d88_file_native_available", "d88_gui_jobs_context"})


if __name__ == "__main__":
    unittest.main()
