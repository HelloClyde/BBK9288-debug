"""Compile and execute production probe.c with SDK/address-space models."""
from __future__ import annotations
import ctypes as C
import os
from pathlib import Path
import shutil
import struct
import subprocess
import unittest
import zlib

from compiler_paths import native_gcc

ROOT=Path(__file__).resolve().parents[1]

class ProbeRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=Path(native_gcc())
        if not compiler.is_file():
            found=shutil.which("gcc")
            if not found: raise unittest.SkipTest("Native GCC is required for production probe tests")
            compiler=Path(found)
        build=ROOT/"build/tests";build.mkdir(parents=True,exist_ok=True)
        output=build/f"probe-runtime-{os.getpid()}.{'dll' if os.name=='nt' else 'so'}"
        env=dict(os.environ);env["PATH"]=str(compiler.parent)+os.pathsep+env.get("PATH","")
        command=[str(compiler),"-std=c99","-O2","-Wall","-Wextra","-Werror","-shared",
                 "-I",str(ROOT/"tests/probe_stubs"),str(ROOT/"tests/probe_runtime_model.c"),"-o",str(output)]
        if os.name!='nt':command.insert(1,"-fPIC")
        built=subprocess.run(command,capture_output=True,text=True,env=env)
        if built.returncode:raise RuntimeError(built.stdout+built.stderr)
        cls.lib=C.CDLL(str(output))
        cls.lib.probe_model_reset.argtypes=[C.c_uint,C.c_uint,C.c_uint]
        cls.lib.probe_model_reset.restype=None
        cls.lib.probe_model_run.restype=C.c_int
        for name in ["title","message"]:
            fn=getattr(cls.lib,"probe_model_"+name);fn.argtypes=[C.c_uint];fn.restype=C.c_char_p
        cls.lib.probe_model_file.argtypes=[C.c_uint];cls.lib.probe_model_file.restype=C.POINTER(C.c_ubyte)
        cls.lib.probe_model_length.argtypes=[C.c_uint];cls.lib.probe_model_length.restype=C.c_uint

    def run_probe(self,profile=1,error=0,existing=0):
        self.lib.probe_model_reset(profile,error,existing)
        return self.lib.probe_model_run()
    def dialogs(self):
        return [(self.lib.probe_model_title(i).decode(),self.lib.probe_model_message(i).decode())
                for i in range(self.lib.probe_model_dialog_count())]
    def file(self,index=0):
        n=self.lib.probe_model_length(index)
        return C.string_at(self.lib.probe_model_file(index),n)
    def report(self,index=0):
        data=self.file(index);self.assertEqual(len(data),1024)
        words=struct.unpack("<256I",data)
        self.assertEqual(words[:3],(0x50383844,3,1024))
        self.assertEqual(words[255],zlib.crc32(data[:1020])&0xffffffff)
        return words

    def test_known_profile_start_full_report_crc_and_saved(self):
        cases=[(1,0x02260000,0x02240d88,0x022a9fac,0x0228a60c,0x022411b4),
               (2,0x02310000,0x022a9820,0x02069000,0x02049798,0x02000350)]
        for profile,tcb,tcb_pointer,heap_state,phase,flags in cases:
            with self.subTest(profile=profile):
                self.assertEqual(self.run_probe(profile=profile),0)
                words=self.report()
                self.assertEqual((words[3],words[11],words[25],words[26],words[27],words[28],words[29]),
                                 (1,profile,1,8,72,2,7))
                self.assertEqual(words[24],tcb)
                self.assertEqual(words[32],(tcb+72)^0x11223344)
                self.assertEqual(words[32+7*18+6],(tcb+8*72+24)^0x11223344)
                self.assertEqual(words[16],heap_state^0x11223344)
                self.assertEqual(words[193],phase^0x11223344)
                self.assertEqual(words[196],flags&255)
                self.assertEqual((words[224],words[225],words[227],words[230]),
                                 (tcb_pointer,heap_state,phase,flags))
                self.assertEqual(self.lib.probe_model_signature_reads(),120)
                self.assertGreater(self.lib.probe_model_specific_reads(),0)
                self.assertGreater(self.lib.probe_model_mmio_reads(),0)
                self.assertEqual(self.lib.probe_model_write_opens(),2)
                self.assertEqual([x[0] for x in self.dialogs()],["D88PRB3 START","D88PRB3 SAVED"])
                self.assertIn("Full report",self.dialogs()[-1][1])
                self.assertEqual(self.file()[832:848],bytes.fromhex("066805098668ffdfd46d2b1e050f1864"))
                if profile==2:
                    self.assertEqual(words[12:16],(0x021ab7ac,0x021ab886,0x021aba78,0x021abb9a))

    def test_mismatched_profile_saves_base_report_without_specific_reads(self):
        self.assertEqual(self.run_probe(profile=0),0)
        words=self.report()
        self.assertEqual((words[3],words[28]),(0,1))
        self.assertEqual(words[4],0x021807ec)
        self.assertEqual(words[16:28],(0,)*12)
        self.assertEqual(self.lib.probe_model_specific_reads(),0)
        self.assertEqual(self.lib.probe_model_signature_reads(),0)
        self.assertEqual(self.lib.probe_model_mmio_reads(),0)
        self.assertEqual(self.lib.probe_model_write_opens(),1)
        self.assertIn("Table-only",self.dialogs()[-1][1])
        self.assertIn("0x021807EC",self.dialogs()[-1][1])

    def test_matching_table_with_each_mismatched_code_signature_stays_baseline(self):
        for profile in (1,2):
            for signature in range(3):
                with self.subTest(profile=profile,signature=signature):
                    self.assertEqual(self.run_probe(profile=profile,error=9+signature),0)
                    words=self.report()
                    self.assertEqual((words[3],words[11],words[28],words[29]),
                                     (0,profile,1,7^(1<<signature)))
                    self.assertEqual(words[16:28],(0,)*12)
                    self.assertEqual(words[224:231],(0,)*7)
                    self.assertEqual(self.lib.probe_model_specific_reads(),0)
                    self.assertEqual(self.lib.probe_model_mmio_reads(),0)
                    self.assertGreater(self.lib.probe_model_signature_reads(),0)
                    self.assertEqual(self.lib.probe_model_write_opens(),1)
                    self.assertIn("Table-only",self.dialogs()[-1][1])

    def test_existing_reports_preserved_and_first_available_name_selected(self):
        self.assertEqual(self.run_probe(existing=0b00011),0)
        self.assertEqual(self.file(0),b"\xa0"*1024)
        self.assertEqual(self.file(1),b"\xa1"*1024)
        self.report(2)
        self.assertEqual(self.lib.probe_model_preexisting_write_attempts(),0)
        self.assertIn("DBG88P03.BIN",self.dialogs()[-1][1])

    def test_all_five_existing_reports_have_visible_message_and_no_writes(self):
        self.assertEqual(self.run_probe(existing=31),-11)
        self.assertEqual(self.lib.probe_model_write_opens(),0)
        self.assertEqual(self.lib.probe_model_specific_reads(),0)
        self.assertEqual(self.dialogs()[-1][0],"D88PRB3 FILES EXIST")
        for i in range(5):self.assertEqual(self.file(i),bytes([0xa0+i])*1024)

    def test_invalid_fs_function_address_has_visible_error_and_no_io(self):
        self.assertEqual(self.run_probe(error=8),-10)
        self.assertEqual(self.lib.probe_model_write_opens(),0)
        self.assertEqual(self.lib.probe_model_specific_reads(),0)
        self.assertEqual(self.dialogs()[-1][0],"D88PRB3 ERROR")
        self.assertIn("0x0000000A",self.dialogs()[-1][1])

    def test_each_file_failure_reports_stage_and_never_claims_saved(self):
        for fault,status in [(1,-20),(2,-21),(3,-22),(4,-23),(5,-24),(6,-24),(7,-25)]:
            with self.subTest(fault=fault):
                self.assertEqual(self.run_probe(error=fault),status)
                dialogs=self.dialogs()
                self.assertEqual([x[0] for x in dialogs],["D88PRB3 START","D88PRB3 ERROR"])
                self.assertIn(f"0x{-status:08X}",dialogs[-1][1])
                self.assertIn("A:\\DBG88P01.BIN",dialogs[-1][1])
                self.assertEqual(self.lib.probe_model_specific_reads(),0)

if __name__=="__main__":unittest.main()
