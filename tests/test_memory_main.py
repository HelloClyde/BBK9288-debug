"""Exercise production D88MEM sequencing, ownership, verification and cleanup."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc

ROOT=Path(__file__).resolve().parents[1]

class MemoryMainTests(unittest.TestCase):
    def test_frontend(self):
        gcc=native_gcc()
        if not Path(gcc).is_file():self.skipTest('GCC required')
        env=dict(os.environ);env['PATH']=str(Path(gcc).parent)+os.pathsep+env.get('PATH','')
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'main.exe'
            subprocess.run([gcc,'-std=c99','-O2','-Wall','-Wextra','-Werror',
                '-Wno-int-to-pointer-cast','-Wno-pointer-to-int-cast','-fno-delete-null-pointer-checks',
                '-I',str(ROOT/'tests/memory_stubs'),'-I',str(ROOT/'device'),'-I',str(ROOT/'memory'),
                str(ROOT/'tests/memory_main_model.c'),'-o',str(exe)],check=True,capture_output=True,text=True,env=env)
            report=Path(directory)/'report.bin'
            result=subprocess.run([str(exe),str(report)],check=True,capture_output=True,text=True,timeout=20,env=env)
            self.assertIn('PASS',result.stdout)
            from host.memory import decode
            decoded=decode(report.read_bytes(),0x12345678)
            self.assertEqual(decoded['state'],'DONE')
            self.assertEqual(len(decoded['speed_results']),30)
            self.assertEqual(len(decoded['speed_summary']),10)
            self.assertEqual(decoded['allocation']['active'],0)
            bad=bytearray(report.read_bytes());bad[400]^=1
            with self.assertRaisesRegex(ValueError,'CRC32'):
                decode(bad)
