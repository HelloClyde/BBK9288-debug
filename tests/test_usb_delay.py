"""Production-C proof guard for a conservative captured-firmware USB detach."""
import ctypes as C
import _ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc

ROOT = Path(__file__).resolve().parents[1]
GUARD, CONTEXT, INTERRUPTED, CHANGED = -201, -202, -203, -204
SIGNATURES = ((0x021CF58E, 156), (0x021D11B8, 294), (0x02198DBC, 44))


class UsbDelayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = native_gcc()
        if not Path(gcc).is_file():
            raise unittest.SkipTest("GCC is required for the production C model")
        cls.tmp = tempfile.TemporaryDirectory()
        library = Path(cls.tmp.name) / "usb_delay.dll"
        env = dict(os.environ)
        env["PATH"] = str(Path(gcc).parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([gcc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-shared",
                        str(ROOT / "tests/usb_delay_model.c"), "-o", str(library)],
                       check=True, capture_output=True, text=True, env=env)
        cls.lib = C.CDLL(str(library))
        for name in ("model_write8", "model_write32"):
            getattr(cls.lib, name).argtypes = (C.c_uint, C.c_uint)
        for name in ("model_set_psr", "model_set_psr_after", "model_set_advance"):
            getattr(cls.lib, name).argtypes = (C.c_uint,)
        cls.lib.model_set_wait_result.argtypes = (C.c_int,)
        cls.lib.model_mutate_during_wait.argtypes = (C.c_uint, C.c_uint, C.c_uint)
        cls.lib.usb_delay_test_read8.argtypes = (C.c_uint,)
        cls.lib.usb_delay_test_read8.restype = C.c_ubyte

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

    def test_valid_64_units_and_conservative_250_ms_contract(self):
        self.assertEqual(self.lib.d88_usb_detach_delay(), 0)
        self.assertEqual(self.lib.model_waits(), 1)
        self.assertEqual(self.lib.model_last_ticks(), 64)
        self.assertEqual(self.lib.model_detach_ms(), 250)
        # Even allowing an arbitrary starting phase, maximum legal input
        # frequency gives a much longer lower bound than the advertised one.
        self.assertGreater(63 * 292 * 4096 / 48_000_000, 0.250)

    def test_wrong_native_addresses_constant_and_vector_refuse_before_wait(self):
        for address in (0x02000200, 0x021E81E8 + 108, 0x021E81E8 + 216,
                        0x02000098, 0x022A981C):
            with self.subTest(address=hex(address)):
                self.lib.model_reset()
                self.lib.model_write32(address, 0)
                self.assertEqual(self.lib.d88_usb_detach_delay(), GUARD)
                self.assertEqual(self.lib.model_waits(), 0)

    def test_every_code_byte_is_guarded_before_wait(self):
        for address, length in SIGNATURES:
            for offset in range(length):
                at = address + offset
                original = self.lib.usb_delay_test_read8(at)
                self.lib.model_write8(at, original ^ 1)
                self.assertEqual(self.lib.d88_usb_detach_delay(), GUARD, hex(at))
                self.assertEqual(self.lib.model_waits(), 0)
                self.lib.model_write8(at, original)

    def test_invalid_timer_clock_and_legacy_tick_refuse_before_wait(self):
        for address, value in ((0x40149, 0x0E), (0x40149, 7),
                               (0x48192, 0x23), (0x48193, 0),
                               (0x48196, 0), (0x48196, 9),
                               (0x48196, 0x21), (0x48196, 0x41),
                               (0x40273, 0x40), (0x40278, 1), (0x40180, 0x1F)):
            with self.subTest(address=hex(address), value=value):
                self.lib.model_reset()
                self.lib.model_write8(address, value)
                self.assertEqual(self.lib.d88_usb_detach_delay(), GUARD)
                self.assertEqual(self.lib.model_waits(), 0)

    def test_locked_interrupt_or_interrupt_disabled_context_refuses(self):
        for address, value in ((0x0206866C, 1), (0x0206866D, 1),
                               (0x0206866D, 2), (0x0206866D, 4)):
            self.lib.model_reset()
            self.lib.model_write8(address, value)
            self.assertEqual(self.lib.d88_usb_detach_delay(), CONTEXT)
            self.assertEqual(self.lib.model_waits(), 0)
        for psr in (0, 0x700, 0x710, 0x110):
            self.lib.model_reset()
            self.lib.model_set_psr(psr)
            self.assertEqual(self.lib.d88_usb_detach_delay(), CONTEXT)
            self.assertEqual(self.lib.model_waits(), 0)

    def test_native_early_release_or_error_never_claims_elapsed(self):
        for result in (-1, -21, -85, 1):
            self.lib.model_reset()
            self.lib.model_set_wait_result(result)
            self.assertEqual(self.lib.d88_usb_detach_delay(), INTERRUPTED)
            self.assertEqual(self.lib.model_waits(), 1)

    def test_changes_during_wait_refuse_including_valid_clock_source_change(self):
        changes = [(0x40149, 0x0E, 1), (0x48192, 0x23, 1),
                   (0x48196, 9, 1), (0x40273, 0x40, 1), (0x40278, 1, 1),
                   (0x40180, 0x3E, 1), (0x40181, 1, 1), (0x40190, 1, 1),
                   (0x0206866C, 1, 1), (0x0206866D, 2, 1),
                   (0x02000098, 0, 4), (0x022A981C, 2, 4),
                   (0x021CF58E, 0, 1), (0x021D11B8, 0, 1), (0x02198DBC, 1, 1)]
        for address, value, width in changes:
            with self.subTest(address=hex(address)):
                self.lib.model_reset()
                self.lib.model_mutate_during_wait(address, value, width)
                self.assertEqual(self.lib.d88_usb_detach_delay(), CHANGED)
                self.assertEqual(self.lib.model_waits(), 1)
        self.lib.model_reset()
        self.lib.model_set_psr_after(0)
        self.assertEqual(self.lib.d88_usb_detach_delay(), CHANGED)

    def test_native_time_must_advance_and_handles_u32_wrap(self):
        for advance in (0, 1, 63, 0xFFFFFFFF, 0x80000000):
            self.lib.model_reset()
            self.lib.model_set_advance(advance)
            self.assertEqual(self.lib.d88_usb_detach_delay(), CHANGED)
        for advance in (64, 65):
            self.lib.model_reset()
            self.lib.model_write32(0x02068674, 0xFFFFFFF0)
            self.lib.model_set_advance(advance)
            self.assertEqual(self.lib.d88_usb_detach_delay(), 0)

    def test_optional_private_capture_matches_embedded_signatures(self):
        path = ROOT / "reference/hardware-kernel-body.bin"
        if not path.is_file():
            self.skipTest("private captured firmware is not distributed")
        body = path.read_bytes()
        for address, length in SIGNATURES:
            actual = bytes(self.lib.usb_delay_test_read8(address + i) for i in range(length))
            self.assertEqual(actual, body[address - 0x02000000:address - 0x02000000 + length])


if __name__ == "__main__":
    unittest.main()
