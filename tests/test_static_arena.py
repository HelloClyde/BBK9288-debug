"""Guarded native-C arena model and independent IEEE CRC32 check; no hardware."""
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
import zlib

from compiler_paths import native_gcc, target_compiler

ROOT=Path(__file__).resolve().parents[1]


class StaticArenaTests(unittest.TestCase):
    def test_private_bounds_two_patterns_crc_and_cooperative_failures(self):
        gcc=native_gcc()
        if not Path(gcc).is_file():self.skipTest("GCC required")
        env=dict(os.environ);env["PATH"]=str(Path(gcc).parent)+os.pathsep+env.get("PATH","")
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/"arena.exe"
            subprocess.run([gcc,"-std=c99","-O2","-Wall","-Wextra","-Werror",
                str(ROOT/"tests/static_arena_model.c"),"-o",str(exe)],check=True,capture_output=True,text=True,env=env)
            result=subprocess.run([str(exe)],check=True,capture_output=True,text=True,env=env,timeout=20)
        self.assertIn("PASS",result.stdout)
        match=re.search(r"crc round1=([0-9a-f]+) round2=([0-9a-f]+)",result.stdout)
        self.assertIsNotNone(match)
        initial=bytearray()
        for index in range(786432//4):
            value=((index^(index<<9)^(index>>7))+0xa55a3cc3)&0xffffffff
            initial.extend(struct.pack("<I",value))
        self.assertEqual(int(match[1],16),zlib.crc32(initial)&0xffffffff)
        self.assertEqual(int(match[2],16),zlib.crc32(bytes(value^255 for value in initial))&0xffffffff)

    def test_target_strict_compile_has_no_unresolved_runtime(self):
        compiler=target_compiler()
        if not compiler.is_file():self.skipTest("S1C33 compiler required")
        from elftools.elf.elffile import ELFFile
        with tempfile.TemporaryDirectory() as directory:
            obj=Path(directory)/"arena.o"
            subprocess.run([str(compiler),"--target=s1c33-none-elf","-Os","-ffreestanding","-fno-builtin",
                "-Wall","-Wextra","-Werror","-c",str(ROOT/"memory_layout/static_arena.c"),"-o",str(obj)],
                check=True,capture_output=True,text=True)
            with obj.open("rb") as source:
                elf=ELFFile(source)
                undefined=[s.name for s in elf.get_section_by_name(".symtab").iter_symbols()
                           if s.name and s["st_shndx"]=="SHN_UNDEF"]
                self.assertEqual(undefined,[])
                arena=elf.get_section_by_name(".app_arena")
                self.assertIsNotNone(arena)
                self.assertEqual(arena["sh_size"],786432)
                self.assertGreaterEqual(arena["sh_addralign"],16)


if __name__=="__main__":unittest.main()
