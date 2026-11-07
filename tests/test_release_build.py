"""Release packaging checks independent of the private hardware test setup."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.kf2_container import debug_icon, inspect, pack


class ReleaseContainerTests(unittest.TestCase):
    def test_kf2_offsets_title_and_payload_round_trip(self):
        payload = bytes(range(256)) * 3
        data = pack(payload, "D88DEBUG")
        result = inspect(data)
        self.assertEqual(result["code_offset"], 536)
        self.assertEqual(data[536:], payload)
        self.assertEqual(data[:12], bytes.fromhex("4b4632001202761901000900"))
        self.assertEqual(data[12:28], b"D88DEBUG" + bytes(8))

    def test_icon_format_and_original_artwork_are_reproducible(self):
        for size in (16, 40):
            icon = debug_icon(size)
            self.assertEqual(struct.unpack_from("<6H", icon), (size, size, 2, 0, 0, 0))
            self.assertEqual(len(icon), 12 + size * size // 4)
            self.assertEqual(icon, debug_icon(size))
            self.assertNotEqual(len(set(icon[12:])), 1)

    def test_invalid_titles_and_oversize_payload_are_rejected(self):
        for title in ("", "x" * 16, "x\0y", "汉" * 8):
            with self.assertRaises(ValueError):
                pack(b"payload", title)
        for payload in (b"", bytes(1024 * 1024 - 535)):
            with self.assertRaises(ValueError):
                pack(payload, "D88DEBUG")

    def test_container_header_icon_and_truncation_corruption_are_rejected(self):
        data = pack(b"payload", "D88DEBUG")
        for offset in (0, 4, 8, 10, 12, 28, 32, 36, 40, 44, 48):
            changed = bytearray(data)
            changed[offset] ^= 1
            with self.assertRaises(ValueError):
                inspect(bytes(changed))
        with self.assertRaises(ValueError):
            inspect(data[:-1])


if __name__ == "__main__":
    unittest.main()
