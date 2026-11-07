"""Native regression of the production captured-profile GUI input worker."""
import ctypes as C
import _ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc

ROOT = Path(__file__).resolve().parents[1]
ACTIVE = 0x02038474
MAIN, FOCUS, OTHER = 0x02501000, 0x02502000, 0x02503000
QUEUE, OTHER_QUEUE = 0x02504000, 0x02504100
DESKTOP_QUEUE = 0x020383D0
RING = 0x02505000


class InputJobsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = native_gcc()
        if not Path(gcc).is_file():
            raise unittest.SkipTest("GCC is required for the production C model")
        cls.tmp = tempfile.TemporaryDirectory()
        library = Path(cls.tmp.name) / "input_jobs.dll"
        env = dict(os.environ)
        env["PATH"] = str(Path(gcc).parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([gcc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-shared",
                        str(ROOT / "tests/input_jobs_model.c"), "-o", str(library)],
                       check=True, capture_output=True, text=True, env=env)
        cls.lib = C.CDLL(str(library))
        for name in ("model_write8", "model_write32"):
            getattr(cls.lib, name).argtypes = (C.c_uint, C.c_uint)
        cls.lib.model_read32.argtypes = (C.c_uint,)
        cls.lib.model_read32.restype = C.c_uint
        for name in ("model_attempt", "model_accepted_field"):
            getattr(cls.lib, name).argtypes = (C.c_uint, C.c_uint)
            getattr(cls.lib, name).restype = C.c_uint
        cls.lib.d88_input_queue.argtypes = (C.c_uint,)
        cls.lib.model_post_result.argtypes = (C.c_int,)
        cls.lib.model_corrupt_on_lock.argtypes = (C.c_uint,)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "lib"):
            if os.name == "nt":
                _ctypes.FreeLibrary(cls.lib._handle)
            else:
                _ctypes.dlclose(cls.lib._handle)
            del cls.lib
        if hasattr(cls, "tmp"):
            cls.tmp.cleanup()

    def setUp(self):
        self.lib.model_reset()
        self.assertEqual(self.lib.d88_input_available(), 1)

    def tearDown(self):
        self.assertEqual(self.lib.model_lock_depth(), 0)
        self.assertEqual(self.lib.model_unlock_calls(), self.lib.model_lock_calls())

    def accepted(self):
        l = self.lib
        return [tuple(l.model_accepted_field(i, j) for j in range(4))
                for i in range(l.model_accepted())]

    def pair(self, window, scan=30):
        l = self.lib
        self.assertEqual(l.d88_input_queue(scan), 0)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertFalse(l.d88_input_pending())
        self.assertEqual(self.accepted(), [(window, 0x10, scan, 0), (window, 0x12, scan, 0)])

    def test_desktop_main_and_focus_targets(self):
        for active, focus, target in ((0, 0, 0), (MAIN, 0, MAIN), (MAIN, FOCUS, FOCUS)):
            with self.subTest(target=hex(target)):
                self.lib.model_reset()
                self.lib.model_write32(ACTIVE, active)
                self.lib.model_write32(MAIN + 0xC4, focus)
                self.pair(target)

    def test_native_idle_snapshot_structure(self):
        # Read-only original-ROM/captured-kernel emulator boot, 2026-11-04.
        # Evidence: build/io-review/input-native-gui.json. This uses its data
        # with the production reader, not any executing native GUI function.
        l = self.lib
        native_main = 0x0253A3C8
        l.model_write32(ACTIVE, native_main)
        l.model_write32(native_main, 0x00110001)
        l.model_write32(native_main + 0xC4, 0)
        l.model_write32(native_main + 0xDC, DESKTOP_QUEUE)
        l.model_write32(DESKTOP_QUEUE + 0x10, 0x02539F8C)
        l.model_write32(DESKTOP_QUEUE + 0x14, 16)
        l.model_write32(DESKTOP_QUEUE + 0x18, 8)
        l.model_write32(DESKTOP_QUEUE + 0x1C, 8)
        self.pair(native_main)

    def test_up_uses_original_target_after_focus_and_active_change(self):
        l = self.lib
        l.model_write32(MAIN + 0xC4, FOCUS)
        self.assertEqual(l.d88_input_queue(31), 0)
        self.assertEqual(l.d88_input_poll(), 0)
        l.model_write32(ACTIVE, OTHER)
        l.model_write32(MAIN + 0xC4, 0)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual(self.accepted(), [(FOCUS, 0x10, 31, 0), (FOCUS, 0x12, 31, 0)])

    def test_full_ring_before_down_freezes_target_until_retry(self):
        l = self.lib
        l.model_write32(QUEUE + 0x1C, 7)
        self.assertEqual(l.d88_input_queue(32), 0)
        self.assertEqual(l.d88_input_poll(), -2)
        self.assertEqual(l.model_accepted(), 0)
        l.model_write32(ACTIVE, OTHER)
        l.model_write32(QUEUE + 0x18, 3)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual(self.accepted(), [(MAIN, 0x10, 32, 0), (MAIN, 0x12, 32, 0)])

    def test_full_ring_after_down_never_repeats_down(self):
        l = self.lib
        l.model_write32(QUEUE + 0x1C, 6)
        self.assertEqual(l.d88_input_queue(33), 0)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual(l.d88_input_poll(), -2)
        self.assertEqual(l.d88_input_poll(), -2)
        self.assertEqual(l.model_accepted(), 1)
        self.assertEqual([l.model_attempt(i, 1) for i in range(3)], [0x10, 0x12, 0x12])
        l.model_write32(ACTIVE, OTHER)
        l.model_write32(QUEUE + 0x18, 2)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual(self.accepted(), [(MAIN, 0x10, 33, 0), (MAIN, 0x12, 33, 0)])

    def test_destroyed_window_drops_up_without_redirecting(self):
        l = self.lib
        l.model_write32(MAIN + 0xC4, FOCUS)
        self.assertEqual(l.d88_input_queue(34), 0)
        self.assertEqual(l.d88_input_poll(), 0)
        l.model_write32(FOCUS, 0)
        l.model_write32(ACTIVE, OTHER)
        self.assertEqual(l.d88_input_poll(), -1)
        self.assertFalse(l.d88_input_pending())
        self.assertEqual(self.accepted(), [(FOCUS, 0x10, 34, 0)])
        self.assertEqual(l.model_attempts(), 1)

    def test_changed_owner_or_queue_drops_up(self):
        for address, value in ((FOCUS + 0xB8, OTHER), (MAIN + 0xDC, OTHER_QUEUE),
                               (FOCUS, 0x00110001)):
            with self.subTest(address=hex(address)):
                l = self.lib
                l.model_reset()
                l.model_write32(MAIN + 0xC4, FOCUS)
                self.assertEqual(l.d88_input_queue(35), 0)
                self.assertEqual(l.d88_input_poll(), 0)
                l.model_write32(address, value)
                self.assertEqual(l.d88_input_poll(), -1)
                self.assertEqual(l.model_attempts(), 1)

    def test_invalid_window_chain_never_reaches_native_post(self):
        cases = [(ACTIVE, 0x03000000), (ACTIVE, MAIN + 1), (MAIN, 0),
                 (MAIN, 0x00120001), (MAIN + 0xC4, 0x03000000),
                 (FOCUS, 0), (FOCUS + 0xB8, OTHER), (FOCUS + 0xB8, 0),
                 (MAIN + 0xDC, 0), (MAIN + 0xDC, 0x027FFFF0)]
        for address, value in cases:
            with self.subTest(address=hex(address), value=hex(value)):
                l = self.lib
                l.model_reset()
                l.model_write32(MAIN + 0xC4, FOCUS)
                l.model_write32(address, value)
                self.assertEqual(l.d88_input_queue(36), 0)
                self.assertEqual(l.d88_input_poll(), -1)
                self.assertEqual(l.model_attempts(), 0)
                self.assertFalse(l.d88_input_pending())

    def test_invalid_ring_shape_never_reaches_native_post(self):
        for queue in (QUEUE, DESKTOP_QUEUE):
            for offset, value in ((0x10, 0), (0x10, RING + 1), (0x10, 0x027FFFF0),
                                  (0x14, 0), (0x14, 1), (0x14, 1025), (0x14, 0xFFFFFFFF),
                                  (0x18, 8), (0x18, 0xFFFFFFFF), (0x1C, 8)):
                with self.subTest(queue=hex(queue), offset=hex(offset), value=hex(value)):
                    l = self.lib
                    l.model_reset()
                    if queue == DESKTOP_QUEUE:
                        l.model_write32(ACTIVE, 0)
                    l.model_write32(queue + offset, value)
                    self.assertEqual(l.d88_input_queue(37), 0)
                    self.assertEqual(l.d88_input_poll(), -1)
                    self.assertEqual(l.model_attempts(), 0)

    def test_profile_and_native_code_corruption_never_post(self):
        addresses = [0x02000204, 0x021E7B90, 0x021FAA7C, 0x02180FBE, 0x0217C6D8,
                     0x02182352, 0x02182C3A, 0x02182C48, 0x02182C70,
                     0x02182CC8, 0x0218231A, 0x0218234A, 0x021D5120, 0x021D5144]
        for address in addresses:
            with self.subTest(address=hex(address)):
                l = self.lib
                l.model_reset()
                self.assertEqual(l.d88_input_queue(38), 0)
                l.model_write8(address, 0xA5)
                self.assertFalse(l.d88_input_available())
                self.assertEqual(l.d88_input_poll(), -3)
                self.assertEqual(l.model_attempts(), 0)
                self.assertEqual(l.model_lock_calls(), 0)
                self.assertEqual(l.d88_input_queue(39), -1)

    def test_cancel_drops_unposted_down_and_keeps_accepted_up(self):
        l = self.lib
        self.assertEqual(l.d88_input_queue(40), 0)
        l.d88_input_cancel()
        self.assertFalse(l.d88_input_pending())
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual(l.model_attempts(), 0)
        l.model_write32(QUEUE + 0x1C, 7)
        self.assertEqual(l.d88_input_queue(41), 0)
        self.assertEqual(l.d88_input_poll(), -2)
        l.d88_input_cancel()
        self.assertFalse(l.d88_input_pending())
        l.model_write32(QUEUE + 0x1C, 0)
        self.assertEqual(l.d88_input_queue(42), 0)
        self.assertEqual(l.d88_input_poll(), 0)
        l.d88_input_cancel()
        self.assertTrue(l.d88_input_pending())
        self.assertEqual(l.d88_input_queue(43), -2)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual(self.accepted(), [(MAIN, 0x10, 42, 0), (MAIN, 0x12, 42, 0)])

    def test_code_hook_between_initial_check_and_lock_is_rejected(self):
        l = self.lib
        self.assertEqual(l.d88_input_queue(47), 0)
        l.model_corrupt_on_lock(0x02182C3A)
        self.assertEqual(l.d88_input_poll(), -3)
        self.assertEqual(l.model_attempts(), 0)
        self.assertEqual(l.model_lock_calls(), 1)
        self.assertEqual(l.model_unlock_calls(), 1)

    def test_scan_validation_and_busy_job(self):
        l = self.lib
        for scan in (0, 128, 0xFFFFFFFF):
            self.assertEqual(l.d88_input_queue(scan), -1)
        self.assertEqual(l.model_attempts(), 0)
        self.assertEqual(l.d88_input_queue(127), 0)
        self.assertEqual(l.d88_input_queue(1), -2)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual(l.d88_input_poll(), 0)

    def test_lock_failure_is_retryable_and_unlock_failures_are_reported(self):
        l = self.lib
        self.assertEqual(l.d88_input_queue(44), 0)
        l.model_fail_lock()
        self.assertEqual(l.d88_input_poll(), -3)
        self.assertEqual(l.model_attempts(), 0)
        self.assertTrue(l.d88_input_pending())
        self.assertEqual(l.model_unlock_calls(), 0)
        # Reset removes the intentionally unmatched failed lock-call counter.
        l.model_reset()
        self.assertEqual(l.d88_input_queue(44), 0)
        l.model_fail_unlock()
        self.assertEqual(l.d88_input_poll(), -3)
        self.assertEqual(l.model_accepted(), 1)
        self.assertEqual(l.d88_input_poll(), 0)
        self.assertEqual([p[1] for p in self.accepted()], [0x10, 0x12])
        l.model_reset()
        self.assertEqual(l.d88_input_queue(45), 0)
        l.model_write32(ACTIVE, 0x03000000)
        l.model_fail_unlock()
        self.assertEqual(l.d88_input_poll(), -3)
        self.assertEqual(l.d88_input_last_error(), -3)
        self.assertEqual(l.model_attempts(), 0)

    def test_other_native_post_error_drops_job(self):
        l = self.lib
        self.assertEqual(l.d88_input_queue(46), 0)
        l.model_post_result(-7)
        self.assertEqual(l.d88_input_poll(), -7)
        self.assertEqual(l.d88_input_last_error(), -7)
        self.assertFalse(l.d88_input_pending())
        self.assertEqual(l.model_accepted(), 0)


if __name__ == "__main__":
    unittest.main()
