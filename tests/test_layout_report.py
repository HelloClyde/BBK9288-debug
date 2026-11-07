"""D88MAP wire bounds/CRC and completed-capacity claims; no device access."""
import struct,unittest,zlib
from host.layout import decode
def fixture():
    sizes=[16384,16384,4096,76,76,24,80,648,6144]
    raw=bytearray(256+sum(sizes)+4);h=[0]*64
    h[:9]=[0x504d3844,1,len(raw),0x12345678,3,0x100,0,128,63]
    h[18:23]=[9,2359296,1572864,1572864,786432]
    cursor=256
    for i,size in enumerate(sizes):
        h[24+i*4:28+i*4]=[i+1,cursor,size,[1,1,128,1,1,1,1,9,0][i]]
        if i==2:
            for page in range(128):struct.pack_into('<8I',raw,cursor+page*32,0x02000000+page*65536,65536,0,65536,0,0,0,0)
        elif i in (3,4):
            vals=[1,76,0,0x02500000,0x02700000]+[0]*10+[1733036,1734004,0,0]
            struct.pack_into('<19I',raw,cursor,*vals)
        elif i==5:struct.pack_into('<6I',raw,cursor,1,24,0,0x02558000,1572864,0)
        elif i==6:struct.pack_into('<20I',raw,cursor,1,80,0,7,0x02740000,786432,0x02800000,2,393216,393216,192,2,0xfa684127,0x32911f49,0xfa684127,0x32911f49,0,0,0,0)
        cursor+=size
    struct.pack_into('<64I',raw,0,*h);seal(raw);return raw
def seal(raw):struct.pack_into('<I',raw,len(raw)-4,zlib.crc32(raw[:-4])&0xffffffff)
class LayoutReportTests(unittest.TestCase):
    def test_complete_extra_memory_report(self):
        d=decode(fixture(),0x12345678)
        self.assertEqual(d['confirmed_simultaneously_held_bytes'],2359296)
        self.assertEqual(d['state'],'DONE');self.assertEqual(d['iram_changed_bytes'],0)
    def test_bad_crc_and_unpublished_state_rejected(self):
        raw=fixture();raw[500]^=1
        with self.assertRaises(ValueError):decode(raw)
        raw=fixture();struct.pack_into('<I',raw,16,2);seal(raw)
        with self.assertRaises(ValueError):decode(raw)
    def test_overlap_and_wrong_page_bounds_rejected(self):
        raw=fixture();struct.pack_into('<I',raw,116,256);seal(raw)
        with self.assertRaises(ValueError):decode(raw)
        raw=fixture();offset=struct.unpack_from('<I',raw,132)[0];struct.pack_into('<I',raw,offset,0x10000000);seal(raw)
        with self.assertRaises(ValueError):decode(raw)
    def test_done_cannot_claim_unreleased_heap(self):
        raw=fixture();offset=struct.unpack_from('<I',raw,180)[0];struct.pack_into('<I',raw,offset+20,1);seal(raw)
        with self.assertRaises(ValueError):decode(raw)
        with self.assertRaises(ValueError):decode(fixture(),0xffffffff)
if __name__=='__main__':unittest.main()
