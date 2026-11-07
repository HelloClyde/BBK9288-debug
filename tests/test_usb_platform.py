"""Native regression of the production USB claim/restore layer."""
import ctypes as C
import _ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc

ROOT=Path(__file__).resolve().parents[1]
TCB=0x02068B34+3*72

class USBPlatformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc=native_gcc()
        if not Path(gcc).is_file(): raise unittest.SkipTest('GCC is required')
        cls.tmp=tempfile.TemporaryDirectory()
        dll=Path(cls.tmp.name)/'usb_platform.dll'
        env=dict(os.environ);env['PATH']=str(Path(gcc).parent)+os.pathsep+env.get('PATH','')
        subprocess.run([gcc,'-std=c99','-O2','-Wall','-Wextra','-Werror','-shared',
                        str(ROOT/'tests/usb_platform_model.c'),'-o',str(dll)],
                       check=True,capture_output=True,text=True,env=env)
        cls.lib=C.CDLL(str(dll))
        for name in ('model_write8','model_write32','model_protected_write8'):
            getattr(cls.lib,name).argtypes=(C.c_uint,C.c_uint)
        for name in ('model_read8','model_read32','model_calls'):
            getattr(cls.lib,name).argtypes=(C.c_uint,)
            getattr(cls.lib,name).restype=C.c_uint
    @classmethod
    def tearDownClass(cls):
        if hasattr(cls,'lib'):
            if os.name=='nt': _ctypes.FreeLibrary(cls.lib._handle)
            else: _ctypes.dlclose(cls.lib._handle)
            del cls.lib
        if hasattr(cls,'tmp'): cls.tmp.cleanup()
    def setUp(self): self.lib.model_reset()
    def task_state(self): return self.lib.model_read8(TCB+0x3f)
    def assert_native_restored(self):
        l=self.lib
        self.assertEqual(l.usb_platform_get_state(),0)
        self.assertEqual(l.model_read8(0x300f20),0x27)
        self.assertEqual(l.model_read8(0x40273)&0x40,0x40)
        self.assertEqual(l.model_read8(0x402a6)&0x40,0x40)
        self.assertEqual(self.task_state(),7)
        self.assertEqual(l.model_read32(0x020000a8),0x0219c9ac)
        self.assertEqual(l.model_lock_depth(),0)
        self.assertEqual(l.model_nested_locks(),0)
        self.assertEqual(l.model_powerup_violations(),0)
    def test_complete_takeover_and_shared_sram_restore(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0)
        self.assertEqual(l.usb_platform_get_state(),2)
        self.assertEqual(self.task_state(),15)
        self.assertEqual(l.model_read8(0x300f20),0x17)
        self.assertEqual(l.model_read8(0x40273),4)
        self.assertEqual(l.model_read32(0x020000a8),0x02500100)
        self.assertEqual(l.usb_platform_check(),0)
        l.model_modify_cdc()
        l.model_write8(0x40283,0x44)
        l.model_write8(0x402a9,0xe0)
        # Other peripherals' enable bits may change while the service runs.
        l.model_write8(0x40273,6);l.model_write8(0x402a6,4)
        self.assertEqual(l.usb_platform_release(),0)
        self.assert_native_restored()
        self.assertEqual(l.model_read8(0x40273),0x46)
        self.assertEqual(l.model_read8(0x402a6),0x44)
        self.assertEqual(l.model_read8(0x40283),4)
        self.assertEqual(l.model_read8(0x402a9),0xa0)
        self.assertTrue(l.model_sram_restored())
        self.assertEqual(l.model_calls(3),1);self.assertEqual(l.model_calls(4),1)
        self.assertEqual(l.usb_platform_release(),0)
        self.assertEqual(l.model_calls(4),1)
    def test_busy_native_session_is_never_claimed(self):
        l=self.lib
        for address,value in [(0x24ef004,1),(0x2049798,1),(0x20497e0,1),(0x2000351,1)]:
            with self.subTest(address=hex(address)):
                l.model_reset();l.model_write32(address,value)
                self.assertEqual(l.usb_platform_claim(),-31)
                self.assertEqual(l.model_writes(),0);self.assertEqual(l.model_calls(3),0)
                self.assertEqual(self.task_state(),7)
    def test_wrong_profile_or_task_is_rejected_before_mutation(self):
        l=self.lib
        for address,value in [(0x2000200,0),(0x219c1f2,0),(0x22a9864,0),(TCB+0x18,0),(TCB+0x3f,15)]:
            with self.subTest(address=hex(address)):
                l.model_reset();l.model_write32(address,value)
                self.assertLess(l.usb_platform_claim(),0)
                self.assertEqual(l.model_writes(),0);self.assertEqual(l.model_calls(3),0)
        l.model_reset();l.model_write32(0x2000200,0)
        self.assertEqual(l.usb_platform_claim(),-30)
        self.assertEqual(l.model_mmio_reads(),0)
    def test_bus_or_cold_state_mismatch_is_rejected(self):
        l=self.lib
        for a,v in [(0x48133,0),(0x48124,0),(0x4812e,0),(0x300f35,0x80),(0x300f20,0x17)]:
            with self.subTest(address=hex(a)):
                l.model_reset();l.model_write8(a,v)
                self.assertLess(l.usb_platform_claim(),0)
                self.assertEqual(l.model_writes(),0);self.assertEqual(l.model_calls(3),0)
    def test_suspend_failure_has_no_unowned_resume_or_controller_write(self):
        l=self.lib
        l.model_fail_operation(3)
        self.assertEqual(l.usb_platform_claim(),-32)
        self.assertEqual(l.model_calls(4),0);self.assertEqual(l.model_writes(),0)
        self.assert_native_restored()
        l.model_reset();l.model_no_suspend_effect()
        self.assertEqual(l.usb_platform_claim(),-32)
        self.assertEqual(l.model_calls(4),0);self.assert_native_restored()
    def test_cable_arrival_after_claim_uses_timed_soft_detach(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0)
        l.model_cable_race(1)
        self.assertEqual(l.usb_platform_release(),1)
        self.assertEqual(l.usb_platform_get_state(),3)
        self.assertEqual(self.task_state(),15);self.assertEqual(l.model_calls(4),0)
        self.assertEqual(l.usb_platform_finish_release(249),1)
        self.assertEqual(l.usb_platform_finish_release(250),0)
        self.assert_native_restored()
    def test_powered_cold_idle_can_be_claimed_without_touching_active_msc(self):
        l=self.lib
        l.model_write8(0x1000022,0xc0)
        self.assertEqual(l.usb_platform_claim(),0)
        self.assertEqual(l.usb_platform_check(),0)
        self.assertEqual(self.task_state(),15)
        for address,value in [(0x2049798,1),(0x20497e0,1),(0x24ef004,1),
                              (0x2000351,1),(TCB+0x3f,3),(0x300f20,0x17)]:
            with self.subTest(address=hex(address)):
                l.model_reset();l.model_write8(0x1000022,0xc0)
                l.model_write32(address,value)
                self.assertEqual(l.usb_platform_claim(),-31)
                self.assertEqual(l.model_writes(),0)
                self.assertEqual(l.model_calls(3),0)
    def test_powered_stop_requires_elapsed_detach_then_restores_without_unplug(self):
        l=self.lib
        l.model_write8(0x1000022,0xc0)
        self.assertEqual(l.usb_platform_claim(),0);l.model_modify_cdc()
        self.assertEqual(l.usb_platform_finish_release(250),-31)
        self.assertEqual(l.usb_platform_release(),1)
        self.assertEqual(l.model_read8(0x1000021),0)
        self.assertEqual(l.model_read8(0x1000023)&0x80,0)
        writes=l.model_writes()
        for _ in range(4):
            self.assertEqual(l.usb_platform_release(),1)
            self.assertEqual(l.usb_platform_finish_release(249),1)
        self.assertEqual(l.model_writes(),writes)
        self.assertEqual(l.model_calls(4),0)
        self.assertEqual(l.usb_platform_finish_release(250),0)
        self.assertEqual(l.model_read8(0x1000022),0xc0)
        self.assertTrue(l.model_sram_restored());self.assert_native_restored()
        self.assertEqual(l.usb_platform_release(),0)
        self.assertEqual(l.usb_platform_finish_release(0),0)
    def test_normal_stop_detaches_but_waits_for_physical_unplug(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0);l.model_modify_cdc()
        l.model_write8(0x1000022,0xc0)
        self.assertEqual(l.usb_platform_release(),1)
        self.assertEqual(l.model_read8(0x1000021),0)
        self.assertEqual(l.model_read8(0x1000023)&0x80,0)
        self.assertEqual(self.task_state(),15);self.assertFalse(l.model_sram_restored())
        l.model_disconnect();self.assertEqual(l.usb_platform_finish_release(0),0)
        self.assertTrue(l.model_sram_restored());self.assert_native_restored()
    def test_snapshot_read_error_rolls_back_without_sram_rewrite(self):
        l=self.lib
        l.model_corrupt_cycle(2)
        self.assertEqual(l.usb_platform_claim(),-35)
        self.assertTrue(l.model_sram_restored());self.assert_native_restored()
    def test_restore_error_keeps_native_suspended_and_does_not_replay(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0);l.model_modify_cdc();l.model_drop_sram_write()
        self.assertEqual(l.usb_platform_release(),-37)
        self.assertEqual(l.usb_platform_get_state(),5)
        self.assertEqual(self.task_state(),15);self.assertEqual(l.model_calls(4),0)
        writes=l.model_writes();self.assertEqual(l.usb_platform_release(),-37)
        self.assertEqual(l.model_writes(),writes)
    def test_ownership_health_detects_native_or_irq_interference(self):
        l=self.lib
        for a,v in [(0x20000a8,0x0219c9ac),(0x402a6,0x40),(TCB+0x3f,7),(0x2049798,1),(0x300f20,0x27)]:
            with self.subTest(address=hex(a)):
                l.model_reset();self.assertEqual(l.usb_platform_claim(),0)
                l.model_write32(a,v)
                self.assertEqual(l.usb_platform_check(),-36)
    def test_gui_may_reenable_timer_but_vector_stays_owned(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0)
        l.model_write8(0x40273,0x44)
        self.assertEqual(l.usb_platform_check(),0)
        self.assertEqual(l.model_read32(0x20000a8),0x02500100)
        self.assertEqual(l.usb_platform_release(),0)
        self.assert_native_restored()
    def test_foreign_vector_is_not_overwritten_during_release(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0)
        l.model_write32(0x20000a8,0x02500200)
        self.assertEqual(l.usb_platform_release(),-36)
        self.assertEqual(l.model_read32(0x20000a8),0x02500200)
        self.assertEqual(self.task_state(),15)
    def test_resume_failure_remains_faulted_and_does_not_unmask(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0);l.model_fail_operation(4)
        self.assertEqual(l.usb_platform_release(),-37)
        self.assertEqual(self.task_state(),15)
        self.assertEqual(l.model_read8(0x40273)&0x40,0)
        self.assertEqual(l.model_read8(0x402a6)&0x40,0)
        self.assertEqual(l.model_lock_depth(),0)
    def test_misc_key_is_required_and_authorizes_only_one_write(self):
        l=self.lib
        l.model_protected_write8(0x300f20,0x17)
        self.assertEqual(l.model_read8(0x300f20),0x27)
        l.model_protected_write8(0x300f2f,0x96)
        l.model_protected_write8(0x300f30,0x42)
        l.model_protected_write8(0x300f20,0x17)
        self.assertEqual(l.model_read8(0x300f20),0x27)
        l.model_protected_write8(0x300f2f,0x96)
        l.model_protected_write8(0x300f20,0x17)
        self.assertEqual(l.model_read8(0x300f20),0x17)
        l.model_protected_write8(0x300f20,0x27)
        self.assertEqual(l.model_read8(0x300f20),0x17)
        self.assertEqual(l.model_misc_ignored(),3)
    def test_short_misc_lock_prevents_native_write_consuming_key(self):
        l=self.lib
        l.model_misc_competitor()
        l.model_protected_write8(0x300f2f,0x96)
        l.model_protected_write8(0x300f20,0x17)
        self.assertEqual(l.model_read8(0x300f20),0x27)
        self.assertEqual(l.model_competitor_runs(),1)
        l.model_reset();l.model_misc_competitor()
        self.assertEqual(l.usb_platform_claim(),0)
        self.assertEqual(l.model_read8(0x300f20),0x17)
        self.assertEqual(l.model_misc_ignored(),0)
        self.assertEqual(l.model_competitor_runs(),1)
        self.assertEqual(l.usb_platform_release(),0);self.assert_native_restored()
    def test_ignored_first_misc_write_rolls_back_without_controller_change(self):
        l=self.lib
        l.model_write8(0x1000022,0xc0);l.model_drop_misc_attempt(1)
        self.assertEqual(l.usb_platform_claim(),-34)
        self.assertEqual(l.model_misc_failure(),0x3727)
        self.assert_native_restored()
        self.assertEqual(l.usb_platform_release(),0)
        self.assertEqual(l.model_misc_failure(),0x3727)
    def test_second_misc_write_failure_restores_clock_and_retains_diagnosis(self):
        l=self.lib
        l.model_drop_misc_attempt(2)
        self.assertEqual(l.usb_platform_claim(),-34)
        self.assert_native_restored()
        self.assertEqual(l.model_misc_failure(),0x1737)
        self.assertEqual(l.model_misc_attempts(),4)
    def test_misc_restore_failure_keeps_native_guarded_and_is_not_replayed(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0)
        l.model_drop_misc_attempt(l.model_misc_attempts()+1)
        self.assertEqual(l.usb_platform_release(),-34)
        self.assertEqual(l.model_misc_failure(),0x3717)
        self.assertEqual(l.usb_platform_get_state(),5)
        self.assertEqual(self.task_state(),15)
        self.assertEqual(l.model_read32(0x20000a8),0x02500100)
        writes=l.model_writes()
        self.assertEqual(l.usb_platform_release(),-34)
        self.assertEqual(l.model_writes(),writes)
    def test_snooze_does_not_reset_controller_registers_or_shared_sram(self):
        l=self.lib
        l.model_write8(0x1000052,0x9b)
        self.assertEqual(l.usb_platform_claim(),0)
        self.assertEqual(l.model_read8(0x1000052),0x9b)
        self.assertTrue(l.model_sram_restored())
        self.assertEqual(l.usb_platform_release(),0)
        self.assertEqual(l.model_read8(0x1000052),0x9b)
        self.assertTrue(l.model_sram_restored());self.assert_native_restored()
    def test_misc_helper_lock_failure_retains_guard_without_nested_lock(self):
        l=self.lib
        l.model_fail_operation_after(1,1)
        self.assertEqual(l.usb_platform_claim(),-33)
        self.assertEqual(l.usb_platform_get_state(),5)
        self.assertEqual(l.model_misc_attempts(),0)
        self.assertEqual(l.model_nested_locks(),0)
        self.assertEqual(self.task_state(),15)
        self.assertEqual(l.model_read32(0x20000a8),0x02500100)
    def test_misc_helper_unlock_failure_never_reenters_lock_or_cleanup(self):
        l=self.lib
        l.model_fail_operation_after(2,1)
        self.assertEqual(l.usb_platform_claim(),-33)
        self.assertEqual(l.usb_platform_get_state(),5)
        self.assertEqual(l.model_lock_depth(),1)
        self.assertEqual(l.model_nested_locks(),0)
        writes=l.model_writes()
        self.assertEqual(l.usb_platform_release(),-33)
        self.assertEqual(l.model_writes(),writes)
    def test_cold_restore_requires_controller_active_and_pullup_readback_off(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0);l.model_modify_cdc()
        l.model_drop_usb_quiesce()
        self.assertEqual(l.usb_platform_release(),-34)
        self.assertEqual(l.usb_platform_get_state(),5)
        self.assertEqual(self.task_state(),15)
        self.assertEqual(l.model_read8(0x300f20),0x17)
        self.assertEqual(l.model_read32(0x20000a8),0x02500100)
    def test_invalid_clock_or_revision_cleanup_has_zero_controller_writes(self):
        l=self.lib
        for address,value in [(0x300f20,0x27),(0x1000020,0)]:
            with self.subTest(address=hex(address)):
                l.model_reset();self.assertEqual(l.usb_platform_claim(),0)
                l.model_modify_cdc();l.model_write8(address,value)
                writes=l.model_writes()
                usb_writes=l.model_usb_writes()
                self.assertEqual(l.usb_platform_release(),-34)
                self.assertEqual(l.model_writes(),writes)
                self.assertEqual(l.model_usb_writes(),usb_writes)
                self.assertEqual(l.usb_platform_get_state(),5)
                self.assertEqual(self.task_state(),15)
                self.assertEqual(l.model_read32(0x20000a8),0x02500100)
    def test_wrong_revision_at_claim_never_writes_controller(self):
        l=self.lib
        l.model_write8(0x1000020,0)
        self.assertEqual(l.usb_platform_claim(),-34)
        self.assertEqual(l.model_usb_writes(),0)
        self.assertEqual(l.model_read8(0x1000023),0x81)
        self.assertEqual(l.usb_platform_get_state(),5)
        self.assertEqual(self.task_state(),15)
    def test_clock_powerup_wait_holds_snooze_without_usb_access_or_cpu_lock(self):
        l=self.lib
        self.assertEqual(l.usb_platform_claim(),0)
        self.assertEqual(l.model_powerup_calls(),1)
        self.assertEqual(l.model_powerup_violations(),0)
        self.assertEqual(l.model_powerup_usb_reads(),0)
        self.assertEqual(l.model_powerup_usb_writes(),0)
        self.assertEqual(l.model_read8(0x300f20),0x17)
        self.assertEqual(l.usb_platform_release(),0)
        self.assertEqual(l.model_powerup_calls(),1)
        self.assert_native_restored()
    def test_powerup_delay_failure_rolls_clock_back_before_any_usb_access(self):
        l=self.lib
        l.model_powerup_failure(-203)
        self.assertEqual(l.usb_platform_claim(),-203)
        self.assertEqual(l.model_powerup_calls(),1)
        self.assertEqual(l.model_powerup_usb_reads(),0)
        self.assertEqual(l.model_usb_writes(),0)
        self.assertEqual(l.model_misc_failure(),0)
        self.assert_native_restored()
    def test_ignored_clock_enable_never_calls_powerup_delay(self):
        l=self.lib
        l.model_drop_misc_attempt(1)
        self.assertEqual(l.usb_platform_claim(),-34)
        self.assertEqual(l.model_powerup_calls(),0)
        self.assertEqual(l.model_misc_failure(),0x3727)
        self.assert_native_restored()
    def test_clock_lost_between_wake_stages_cannot_bypass_powerup_wait(self):
        l=self.lib
        l.model_drop_clock_at_lock(3)
        self.assertEqual(l.usb_platform_claim(),-34)
        self.assertEqual(l.model_misc_failure(),0x1727)
        self.assertEqual(l.model_powerup_calls(),2)
        self.assertEqual(l.model_usb_writes(),0)
        self.assert_native_restored()
    def test_restore_repeats_powerup_only_if_clock_was_lost(self):
        l=self.lib
        l.model_write8(0x1000022,0xc0)
        self.assertEqual(l.usb_platform_claim(),0)
        self.assertEqual(l.usb_platform_release(),1)
        l.model_write8(0x300f20,0x27)
        self.assertEqual(l.usb_platform_finish_release(250),0)
        self.assertEqual(l.model_powerup_calls(),2)
        self.assert_native_restored()

if __name__=='__main__': unittest.main()
