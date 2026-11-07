"""USB log decoder boundaries and production launcher native-FS persistence."""
import ctypes as C
import _ctypes
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

from compiler_paths import native_gcc

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("inspect_usb_log", ROOT / "inspect_usb_log.py")
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


def report(count=2):
    words = [0] * 256
    words[:8] = [decoder.MAGIC, 1, 1024, 0x12345678, 3, count, 7, 0]
    words[8:25] = list(range(17))
    words[8 + 9] = 6
    words[8 + 10] = 0xFFFFFFF4
    words[32:59] = list(range(27))
    words[34] = 0xFFFFFFF3
    words[35] = 0x84
    for i in range(min(count, 88)):
        words[64 + i * 2:66 + i * 2] = [0x01000000 + i, i]
    data = bytearray(struct.pack("<256I", *words))
    seal(data)
    return data


def seal(data):
    struct.pack_into("<I", data, 1020, zlib.crc32(data[:1020]))


class UsbLogDecoderTests(unittest.TestCase):
    def test_complete_named_fields_and_exact_offsets(self):
        data = report()
        result = decoder.decode(data)
        self.assertEqual(set(result["control"]), set(decoder.CONTROL_FIELDS))
        self.assertEqual(len(result["control"]), 17)
        self.assertEqual(len(result["diagnostic"]), 27)
        self.assertEqual(result["control"]["stop_address"], 16)
        self.assertEqual(result["diagnostic"]["poll_count"], 26)
        self.assertEqual(result["control_state"], "FAILED")
        self.assertEqual(result["control_last_error_signed"], -12)
        self.assertEqual(result["diagnostic_result_signed"], -13)
        self.assertEqual(result["diagnostic_failures"], ["CLOCK_MISSING", "REVISION"])
        self.assertEqual(result["register_map"], {"0x01000000": "0x00000000", "0x01000001": "0x00000001"})
        self.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())

    def test_register_counts_zero_and_88_include_last_pair_at_952(self):
        for count in (0, 88):
            result = decoder.decode(report(count))
            self.assertEqual(len(result["registers"]), count)
            if count:
                self.assertEqual(result["registers"][-1], {"address": "0x01000057", "value": "0x00000057"})
        with self.assertRaisesRegex(ValueError, "register_count"):
            decoder.decode(report(89))

    def test_wrong_magic_version_and_header_length(self):
        for offset, value in ((0, 0), (4, 2), (8, 1020)):
            data = report()
            struct.pack_into("<I", data, offset, value)
            seal(data)
            with self.assertRaisesRegex(ValueError, "magic/version/length"):
                decoder.decode(data)

    def test_truncation_and_trailing_bytes_rejected(self):
        for data in (b"", report()[:-1], report() + b"\0"):
            with self.assertRaisesRegex(ValueError, "size"):
                decoder.decode(data)

    def test_crc_detects_header_payload_padding_and_crc_damage(self):
        for offset in (12, 32, 100, 128, 240, 256, 959, 960, 1019, 1020, 1023):
            data = report()
            data[offset] ^= 1
            with self.assertRaisesRegex(ValueError, "CRC32"):
                decoder.decode(data)

    def test_duplicates_preserved_in_ordered_register_list(self):
        data = report()
        struct.pack_into("<II", data, 264, 0x01000000, 0xAB)
        seal(data)
        result = decoder.decode(data)
        self.assertEqual(len(result["registers"]), 2)
        self.assertEqual(result["register_map"]["0x01000000"], "0x000000ab")

    def test_file_and_cli_output_does_not_overwrite_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path, output = Path(directory) / "D88U01.BIN", Path(directory) / "log.json"
            path.write_bytes(report())
            result = decoder.inspect(path)
            self.assertEqual(result["file"], str(path.resolve()))
            run = subprocess.run([os.sys.executable, str(ROOT / "inspect_usb_log.py"), str(path), "--output", str(output)], capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            original = output.read_bytes()
            run = subprocess.run([os.sys.executable, str(ROOT / "inspect_usb_log.py"), str(path), "--output", str(output)], capture_output=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(output.read_bytes(), original)


class UsbLogSaverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = native_gcc()
        if not Path(gcc).is_file():
            raise unittest.SkipTest("GCC is required for the production native-FS model")
        cls.tmp = tempfile.TemporaryDirectory()
        library = Path(cls.tmp.name) / "usb_log.dll"
        env = dict(os.environ)
        env["PATH"] = str(Path(gcc).parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([gcc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-shared",
                        "-I", str(ROOT / "tests/probe_stubs"), str(ROOT / "tests/usb_log_model.c"),
                        "-o", str(library)], check=True, capture_output=True, env=env)
        cls.lib = C.CDLL(str(library))
        cls.lib.model_reset.argtypes = (C.c_uint,)
        cls.lib.model_save.argtypes = (C.c_char_p,)
        cls.lib.model_null_report.argtypes = (C.c_char_p,)
        for name in ("model_preexist", "model_reg_count", "model_mutate_input_on_open"):
            getattr(cls.lib, name).argtypes = (C.c_uint,)
        cls.lib.model_data.argtypes = (C.c_uint,)
        cls.lib.model_data.restype = C.POINTER(C.c_ubyte)
        cls.lib.model_input_word.argtypes = (C.c_uint,)
        cls.lib.model_input_word.restype = C.c_uint
        cls.lib.model_length.argtypes = (C.c_uint,)

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
        self.lib.model_reset(0)
        self.path = C.create_string_buffer(24)

    def data(self, index):
        return C.string_at(self.lib.model_data(index), self.lib.model_length(index))

    def test_production_report_round_trips_decoder_and_readback(self):
        self.assertEqual(self.lib.model_save(self.path), 0)
        self.assertEqual(self.path.value, b"A:\\D88U01.BIN")
        result = decoder.decode(self.data(0))
        self.assertEqual(result["image_id"], "12345678")
        self.assertEqual(result["diagnostic"]["failure_mask"], 0x203)
        self.assertEqual(result["register_map"]["0x00300f20"], "0x00000017")
        self.assertEqual(self.lib.model_input_word(0), 0)
        self.assertEqual(self.lib.model_close_calls(), 2)

    def test_first_hole_and_slot_99_never_write_preexisting_reports(self):
        for i in (0, 2):
            self.lib.model_preexist(i)
        old0, old2 = self.data(0), self.data(2)
        self.assertEqual(self.lib.model_save(self.path), 0)
        self.assertEqual(self.path.value, b"A:\\D88U02.BIN")
        self.assertEqual((self.data(0), self.data(2)), (old0, old2))
        self.lib.model_reset(0)
        for i in range(98):
            self.lib.model_preexist(i)
        self.assertEqual(self.lib.model_save(self.path), 0)
        self.assertEqual(self.path.value, b"A:\\D88U99.BIN")
        self.assertEqual(self.lib.model_old_write_attempts(), 0)

    def test_all_99_slots_full_refuses_without_write(self):
        for i in range(99):
            self.lib.model_preexist(i)
        self.assertEqual(self.lib.model_save(self.path), -11)
        self.assertEqual(self.path.value, b"")
        self.assertEqual(self.lib.model_write_opens(), 0)

    def test_every_save_and_readback_failure_has_error_and_selected_path(self):
        for fault, result in ((1, -20), (2, -21), (3, -22), (4, -23), (5, -24), (6, -24), (7, -25)):
            with self.subTest(fault=fault):
                self.lib.model_reset(fault)
                self.assertEqual(self.lib.model_save(self.path), result)
                self.assertEqual(self.path.value, b"A:\\D88U01.BIN")
                self.assertEqual(self.lib.model_old_write_attempts(), 0)

    def test_existing_close_failure_and_bad_fs_never_open_for_write(self):
        for fault, result in ((8, -12), (9, -10), (10, -10)):
            self.lib.model_reset(fault)
            self.lib.model_preexist(0)
            self.assertEqual(self.lib.model_save(self.path), result)
            self.assertEqual(self.path.value, b"")
            self.assertEqual(self.lib.model_write_opens(), 0)

    def test_copy_is_taken_before_native_fs_callbacks(self):
        self.lib.model_mutate_input_on_open(1)
        self.assertEqual(self.lib.model_save(self.path), 0)
        self.assertEqual(self.lib.model_input_word(35), 0xDEADBEEF)
        self.assertEqual(decoder.decode(self.data(0))["diagnostic"]["failure_mask"], 0x203)

    def test_invalid_argument_and_register_count_boundaries(self):
        self.path.value = b"unchanged"
        self.assertEqual(self.lib.model_null_report(self.path), -1)
        self.assertEqual(self.path.value, b"")
        self.assertEqual(self.lib.model_null_path(), -1)
        self.lib.model_reg_count(89)
        self.assertEqual(self.lib.model_save(self.path), -1)
        self.assertEqual(self.lib.model_write_opens(), 0)
        for count in (0, 88):
            self.lib.model_reset(0)
            self.lib.model_reg_count(count)
            self.assertEqual(self.lib.model_save(self.path), 0)
            self.assertEqual(decoder.decode(self.data(0))["header"]["register_count"], count)

    def test_partial_report_is_retained_and_next_call_uses_next_slot(self):
        self.lib.model_reset(2)
        self.assertEqual(self.lib.model_save(self.path), -21)
        old = self.data(0)
        self.assertEqual(self.lib.model_save(self.path), -21)
        self.assertEqual(self.path.value, b"A:\\D88U02.BIN")
        self.assertEqual(self.data(0), old)


if __name__ == "__main__":
    unittest.main()
