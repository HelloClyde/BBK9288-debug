import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

spec=importlib.util.spec_from_file_location("inspect_probe",Path(__file__).resolve().parents[1]/"inspect_probe.py")
probe=importlib.util.module_from_spec(spec); spec.loader.exec_module(probe)

class ProbeTests(unittest.TestCase):
    def make(self):
        words=[0]*256
        words[:3]=[0x50383844,1,1024]
        words[4:9]=[0x021806ec,0x02180058,0x0217ffe4,0x0217fed4,0x0200021c]
        words[25:28]=[1,8,72]
        data=bytearray(struct.pack("<256I",*words))
        struct.pack_into("<I",data,128+7*72+24,0x02123456)
        return data
    def test_eight_tasks_including_slot_eight(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/"probe.bin"; path.write_bytes(self.make())
            result=probe.inspect(path)
            self.assertEqual([x["id"] for x in result["tasks"]],list(range(1,9)))
            self.assertEqual(result["tasks"][-1]["entry"],"0x02123456")
            self.assertFalse(result["hardware_usb_verified"])
    def test_truncated_or_wrong_profile_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/"probe.bin"
            for data in [self.make()[:-1],b"\0"*1024]:
                path.write_bytes(data)
                with self.assertRaises(ValueError): probe.inspect(path)
    def test_v2_unknown_profile_baseline_is_useful(self):
        data=self.make()
        struct.pack_into("<I",data,4,2)
        struct.pack_into("<I",data,16,0x02189900)
        struct.pack_into("<I",data,28*4,1)
        struct.pack_into("<I",data,1020,zlib.crc32(data[:1020]))
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/"probe.bin";path.write_bytes(data)
            result=probe.inspect(path)
            self.assertFalse(result["profile_match"])
            self.assertEqual(result["report_kind"],"table-only")
            self.assertEqual(result["tasks"],[])
            self.assertEqual(result["tables"]["ros33"],"0x02189900")
            data[500]^=1;path.write_bytes(data)
            with self.assertRaisesRegex(ValueError,"CRC32"): probe.inspect(path)
    def test_v3_captured_profile_full_and_signature_rejection(self):
        data=self.make()
        struct.pack_into("<I",data,4,3)
        struct.pack_into("<I",data,12,1)
        struct.pack_into("<6I",data,16,0x021e81e8,0x021e7b54,0x021e7ae0,0x021e79d0,0x0200021c,0x021e7a3c)
        struct.pack_into("<I",data,11*4,2)
        struct.pack_into("<I",data,28*4,2)
        struct.pack_into("<I",data,29*4,7)
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/"probe.bin"
            struct.pack_into("<I",data,1020,zlib.crc32(data[:1020]));path.write_bytes(data)
            result=probe.inspect(path)
            self.assertTrue(result["profile_match"])
            self.assertEqual(result["profile_id"],2)
            self.assertEqual(result["report_kind"],"full")
            self.assertIn("V1.3",result["recognized_profile"])
            struct.pack_into("<I",data,29*4,3)
            struct.pack_into("<I",data,1020,zlib.crc32(data[:1020]));path.write_bytes(data)
            with self.assertRaisesRegex(ValueError,"signature"): probe.inspect(path)
            struct.pack_into("<I",data,12,0)
            struct.pack_into("<I",data,28*4,1)
            struct.pack_into("<I",data,1020,zlib.crc32(data[:1020]));path.write_bytes(data)
            result=probe.inspect(path)
            self.assertFalse(result["profile_match"])
            self.assertEqual(result["report_kind"],"table-only")
            self.assertEqual(result["tasks"],[])

if __name__=="__main__": unittest.main()
