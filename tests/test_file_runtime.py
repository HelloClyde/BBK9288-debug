"""Production file runtime lifecycle and RPC boundaries; no device access."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from compiler_paths import native_gcc

ROOT=Path(__file__).resolve().parents[1]

class FileRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc=native_gcc()
        if not Path(gcc).is_file():raise unittest.SkipTest('GCC required')
        cls.tmp=tempfile.TemporaryDirectory();cls.exe=Path(cls.tmp.name)/'runtime.exe'
        cls.env=dict(os.environ);cls.env['PATH']=str(Path(gcc).parent)+os.pathsep+cls.env.get('PATH','')
        subprocess.run([gcc,'-std=c99','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/file_runtime_model.c'),'-o',str(cls.exe)],check=True,capture_output=True,text=True,env=cls.env)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def case(self,n):
        r=subprocess.run([str(self.exe),str(n)],capture_output=True,text=True,env=self.env,timeout=15)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertIn('PASS',r.stdout)
    def test_start_guards_and_owned_callback(self):self.case(0)
    def test_retry_queue_busy_and_surface_bridge_fault(self):self.case(1)
    def test_submit_reply_does_not_run_fs(self):self.case(2)
    def test_refresh_uses_same_asynchronous_gui_lane(self):self.case(9)
    def test_poll_size_and_job_validation(self):self.case(3)
    def test_cancel_same_job_and_preserve_already_done(self):self.case(4)
    def test_shutdown_waits_for_gui_cleanup_and_unhooks(self):self.case(5)
    def test_shutdown_fault_retains_enabled_image(self):self.case(6)
    def test_cleanup_fault_keeps_recovery_poll_but_rejects_successful_shutdown(self):self.case(7)
    def test_profile_fault_stops_scheduling_and_retains_image(self):self.case(8)
if __name__=='__main__':unittest.main()
