"""Execute the production C resident loader and independently link S1C33 fixtures."""
import ctypes as C
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

from compiler_paths import native_gcc, target_compiler, target_toolchain

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("d88_build_resident", ROOT / "tools/build_resident.py")
BUILD = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = BUILD
SPEC.loader.exec_module(BUILD)


class Record(C.Structure):
    _fields_ = [("offset", C.c_uint), ("target_offset", C.c_uint), ("type", C.c_uint)]


class Blob(C.Structure):
    _fields_ = [("magic", C.c_uint), ("version", C.c_uint), ("data", C.c_void_p),
                ("file_size", C.c_uint), ("memory_size", C.c_uint), ("alignment", C.c_uint),
                ("entry_offset", C.c_uint), ("linked_base", C.c_uint),
                ("relocations", C.c_void_p), ("relocation_count", C.c_uint),
                ("data_crc32", C.c_uint), ("relocation_crc32", C.c_uint)]


class Loaded(C.Structure):
    _fields_ = [("allocation", C.c_void_p), ("memory", C.c_void_p),
                ("size", C.c_uint), ("entry_address", C.c_uint)]


def fixture_image():
    base, target = 0x00100000, 24
    address = base + target
    data = bytearray(16)
    struct.pack_into("<HHH", data, 0, 0xc000 | (address >> 19 & 0x1fff),
                     0xc000 | (address >> 6 & 0x1fff), 0x6c04 | ((address & 63) << 4))
    struct.pack_into("<I", data, 8, address)
    records = [BUILD.Relocation(0, target, 3), BUILD.Relocation(2, target, 4),
               BUILD.Relocation(4, target, 5), BUILD.Relocation(8, target, 2)]
    return BUILD.ResidentImage(bytes(data), base, 64, 16, 0, records, {}, {})


class Descriptor:
    def __init__(self, image):
        self.image = image
        self.data = (C.c_ubyte * len(image.data)).from_buffer_copy(image.data)
        self.records = (Record * max(1, len(image.relocations)))()
        for index, record in enumerate(image.relocations):
            self.records[index] = Record(record.offset, record.target_offset, record.type)
        self.blob = Blob(0x52383844, 1, C.addressof(self.data), len(image.data),
                         image.memory_size, image.alignment, image.entry_offset, image.linked_base,
                         C.addressof(self.records), len(image.relocations), 0, 0)
        self.checksums()

    def checksums(self):
        self.blob.data_crc32 = zlib.crc32(bytes(self.data))
        self.blob.relocation_crc32 = zlib.crc32(bytes(self.records)[:self.blob.relocation_count * 12])


class ResidentLoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = native_gcc()
        if not Path(gcc).is_file():
            raise unittest.SkipTest("GCC is needed for the production C loader model")
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(gcc).parent) + os.pathsep + cls.env.get("PATH", "")
        dll = cls.directory / "resident_loader.dll"
        subprocess.run([gcc, "-shared", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "device/resident_loader.c"), "-o", str(dll)],
                       check=True, capture_output=True, text=True, env=cls.env)
        cls.dll = C.CDLL(str(dll))
        cls.dll.resident_image_validate.argtypes = [C.POINTER(Blob)]
        cls.dll.resident_image_validate.restype = C.c_int
        cls.dll.resident_image_load.argtypes = [C.POINTER(Blob), C.c_void_p, C.c_uint,
                                               C.c_uint, C.POINTER(C.c_uint)]
        cls.dll.resident_image_load.restype = C.c_int
        cls.allocate_type = C.CFUNCTYPE(C.c_void_p, C.c_uint)
        cls.free_type = C.CFUNCTYPE(None, C.c_void_p)
        cls.dll.resident_image_allocate.argtypes = [C.POINTER(Blob), cls.allocate_type,
                                                   cls.free_type, C.POINTER(Loaded)]
        cls.dll.resident_image_allocate.restype = C.c_int

    @classmethod
    def tearDownClass(cls):
        if os.name == "nt":
            import _ctypes
            _ctypes.FreeLibrary(cls.dll._handle)
            cls.dll._handle = 0
        cls.dll = None
        if cls.directory.resolve().parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError("Refusing cleanup outside the test temporary directory")
        cls.temp.cleanup()

    def load(self, descriptor, base=0x02501230, capacity=None):
        memory = (C.c_ubyte * 96)(*([0xa5] * 96))
        entry = C.c_uint(0xdeadbeef)
        result = self.dll.resident_image_load(C.byref(descriptor.blob), C.byref(memory, 8),
                    descriptor.blob.memory_size if capacity is None else capacity, base, C.byref(entry))
        return result, bytes(memory), entry.value

    def test_absolute_instruction_and_pointer_relocation_and_bss(self):
        descriptor = Descriptor(fixture_image())
        self.assertEqual(self.dll.resident_image_validate(C.byref(descriptor.blob)), 0)
        for base in [0x02501230, 0x02634560, 0x024ffff0]:
            with self.subTest(base=hex(base)):
                result, memory, entry = self.load(descriptor, base)
                self.assertEqual(result, 0)
                self.assertEqual(memory[:8], b"\xa5" * 8)
                self.assertEqual(memory[8:24], BUILD.relocate(descriptor.image, base))
                self.assertEqual(memory[24:72], b"\0" * 48)
                self.assertEqual(memory[72:], b"\xa5" * 24)
                self.assertEqual(entry, base)

    def test_every_validation_failure_preserves_destination(self):
        mutations = [
            ("magic", 0), ("version", 99), ("file_size", 0), ("memory_size", 8),
            ("entry_offset", 3), ("entry_offset", 16), ("alignment", 1),
            ("alignment", 3), ("alignment", 0x20000), ("linked_base", 0x100001),
            ("linked_base", 0xfffffff0), ("relocation_count", 0xffffffff),
        ]
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                descriptor = Descriptor(fixture_image())
                setattr(descriptor.blob, field, value)
                result, memory, entry = self.load(descriptor)
                self.assertEqual(result, -1)
                self.assertEqual(memory, b"\xa5" * 96)
                self.assertEqual(entry, 0xdeadbeef)

    def test_crc_failure_in_either_data_or_records(self):
        for where in ("data", "records"):
            descriptor = Descriptor(fixture_image())
            if where == "data": descriptor.data[0] ^= 1
            else: descriptor.records[0].target_offset ^= 1
            result, memory, _ = self.load(descriptor)
            self.assertEqual(result, -2)
            self.assertEqual(memory, b"\xa5" * 96)

    def test_unknown_overlapping_out_of_range_and_wrong_encoding(self):
        for field, value in [("type", 6), ("offset", 15), ("offset", 16),
                             ("offset", 1), ("target_offset", 65)]:
            descriptor = Descriptor(fixture_image())
            setattr(descriptor.records[0], field, value)
            descriptor.checksums()
            result, memory, _ = self.load(descriptor)
            self.assertEqual(result, -3)
            self.assertEqual(memory, b"\xa5" * 96)
        descriptor = Descriptor(fixture_image())
        descriptor.records[1].offset = 0
        descriptor.checksums()
        self.assertEqual(self.load(descriptor)[0], -3)
        descriptor = Descriptor(fixture_image())
        descriptor.data[1] ^= 0x20  # not an EXT instruction
        descriptor.checksums()
        self.assertEqual(self.load(descriptor)[0], -3)
        descriptor = Descriptor(fixture_image())
        descriptor.data[8] ^= 1
        descriptor.checksums()
        self.assertEqual(self.load(descriptor)[0], -3)

    def test_address_capacity_and_source_overlap_are_rejected(self):
        descriptor = Descriptor(fixture_image())
        for base, capacity, error in [(0x02501231, 64, -6), (0xfffffff0, 64, -6),
                                       (0x02501230, 63, -4)]:
            result, memory, _ = self.load(descriptor, base, capacity)
            self.assertEqual(result, error)
            self.assertEqual(memory, b"\xa5" * 96)
        shared = (C.c_ubyte * 96)()
        shared[:16] = descriptor.image.data
        descriptor.blob.data = C.addressof(shared)
        before = bytes(shared)
        self.assertEqual(self.dll.resident_image_load(C.byref(descriptor.blob), C.byref(shared),
                                                      64, 0x02501230, None), -4)
        self.assertEqual(bytes(shared), before)

    def test_one_past_bss_pointer_is_relocated(self):
        image = fixture_image()
        data = bytearray(image.data)
        for record in image.relocations:
            record_value = image.linked_base + image.memory_size
            if record.type == 2: struct.pack_into("<I", data, record.offset, record_value)
            else:
                old = struct.unpack_from("<H", data, record.offset)[0]
                mask = 0x3f0 if record.type == 5 else 0x1fff
                immediate = ((record_value & 63) << 4 if record.type == 5 else
                             record_value >> (19 if record.type == 3 else 6) & 0x1fff)
                struct.pack_into("<H", data, record.offset, old & ~mask | immediate)
        image.data = bytes(data)
        image.relocations = [BUILD.Relocation(r.offset, image.memory_size, r.type) for r in image.relocations]
        descriptor = Descriptor(image)
        self.assertEqual(self.load(descriptor)[0], 0)

    def test_allocator_failure_and_64bit_pointer_roll_back(self):
        descriptor = Descriptor(fixture_image())
        freed, allocated = [], []
        free = self.free_type(lambda address: freed.append(address))
        allocate = self.allocate_type(lambda size: allocated.append(size) or None)
        loaded = Loaded(None, None, 0x12345678, 0x87654321)
        self.assertEqual(self.dll.resident_image_allocate(C.byref(descriptor.blob), allocate,
                                                         free, C.byref(loaded)), -5)
        self.assertEqual(allocated, [79])
        self.assertEqual(freed, [])
        self.assertEqual(loaded.size, 0x12345678)
        if C.sizeof(C.c_void_p) > 4:
            allocate = self.allocate_type(lambda size: 0x100000000)
            self.assertEqual(self.dll.resident_image_allocate(C.byref(descriptor.blob), allocate,
                                                             free, C.byref(loaded)), -6)
            self.assertEqual(freed, [0x100000000])

    def test_allocator_aligns_owns_raw_pointer_and_keeps_live_image(self):
        if os.name != "nt": self.skipTest("Low 32-bit Windows test mapping")
        kernel = C.WinDLL("kernel32", use_last_error=True)
        kernel.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint, C.c_uint]
        kernel.VirtualAlloc.restype = C.c_void_p
        kernel.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, C.c_uint]
        kernel.VirtualFree.restype = C.c_int
        address = None
        for candidate in [0x14000000, 0x18000000, 0x20000000, 0x28000000]:
            address = kernel.VirtualAlloc(candidate, 4096, 0x3000, 4)
            if address: break
        if not address: self.skipTest("No free low-address test mapping")
        try:
            C.memset(address, 0xa5, 4096)
            freed = []
            allocate = self.allocate_type(lambda size: address + 3)
            free = self.free_type(lambda pointer: freed.append(pointer))
            descriptor = Descriptor(fixture_image())
            loaded = Loaded()
            self.assertEqual(self.dll.resident_image_allocate(C.byref(descriptor.blob), allocate,
                                                             free, C.byref(loaded)), 0)
            self.assertEqual(loaded.allocation, address + 3)
            self.assertEqual(loaded.memory, address + 16)
            self.assertEqual(loaded.entry_address, address + 16)
            self.assertEqual(loaded.size, 64)
            self.assertEqual(freed, [])
            self.assertEqual(C.string_at(address + 16, 16), BUILD.relocate(descriptor.image, address + 16))
            self.assertEqual(C.string_at(address + 32, 48), b"\0" * 48)
            self.assertEqual(C.string_at(address, 16), b"\xa5" * 16)
        finally:
            self.assertTrue(kernel.VirtualFree(address, 0, 0x8000))

    def test_real_s1c33_independent_links_and_exports(self):
        clang = target_compiler()
        if not clang.is_file(): self.skipTest("S1C33 compiler is required")
        fixture = self.directory / "actual.c"
        fixture.write_text("""
unsigned bss_state;
unsigned initialized=0x12345678;
unsigned char resident_control[96];
unsigned char resident_stack[8192] __attribute__((aligned(16)));
__attribute__((noinline)) unsigned resident_worker(unsigned n){bss_state+=n;return bss_state;}
unsigned *bss_pointer=&bss_state;
unsigned (*function_pointer)(unsigned)=resident_worker;
unsigned resident_entry(unsigned n){return resident_worker(n)+initialized;}
""", encoding="ascii")
        output = self.directory / "actual"
        image = BUILD.build_resident([fixture], output, toolchain=target_toolchain(),
                    exports=["resident_control", "resident_worker"])
        self.assertTrue({2, 3, 4, 5, 7, 8, 9}.issubset(image.relocation_types))
        self.assertGreaterEqual(image.memory_size - len(image.data), 8192)
        header = (output / "resident_blob.h").read_text()
        self.assertIn("D88_RESIDENT_CONTROL_OFFSET", header)
        self.assertIn("D88_RESIDENT_WORKER_OFFSET", header)
        manifest = json.loads((output / "resident.json").read_text())
        self.assertEqual(manifest["byte_comparison"], "passed")
        descriptor = Descriptor(image)
        for base in BUILD.DEFAULT_VERIFY_BASES:
            destination = (C.c_ubyte * image.memory_size)()
            entry = C.c_uint()
            self.assertEqual(self.dll.resident_image_load(C.byref(descriptor.blob), destination,
                                  image.memory_size, base, C.byref(entry)), 0)
            self.assertEqual(bytes(destination)[:len(image.data)], BUILD.relocate(image, base))
            self.assertEqual(bytes(destination)[len(image.data):], b"\0" * (image.memory_size-len(image.data)))
            self.assertEqual(entry.value, base + image.entry_offset)
        from elftools.elf.elffile import ELFFile
        elf_path = output / "resident.elf"
        with elf_path.open("rb") as stream:
            elf = ELFFile(stream)
            relocation_section = next(s for s in elf.iter_sections() if s["sh_type"] == "SHT_RELA")
            first_info_offset = relocation_section["sh_offset"] + 4
            relative_middle_offset = None
            for section in elf.iter_sections():
                if section["sh_type"] != "SHT_RELA": continue
                for index, relocation in enumerate(section.iter_relocations()):
                    if relocation["r_info_type"] == 8:
                        relative_middle_offset = section["sh_offset"] + index * section["sh_entsize"] + 4
        original = elf_path.read_bytes()
        broken = bytearray(original)
        information = struct.unpack_from("<I", broken, first_info_offset)[0]
        struct.pack_into("<I", broken, first_info_offset, information & ~255 | 6)
        mutant = output / "unknown-relocation.elf"; mutant.write_bytes(broken)
        with self.assertRaisesRegex(BUILD.ResidentBuildError, "Unsupported S1C33"):
            BUILD.read_image(mutant)
        self.assertIsNotNone(relative_middle_offset)
        broken = bytearray(original)
        information = struct.unpack_from("<I", broken, relative_middle_offset)[0]
        struct.pack_into("<I", broken, relative_middle_offset, information & ~255 | 7)
        mutant = output / "incomplete-relative.elf"; mutant.write_bytes(broken)
        with self.assertRaisesRegex(BUILD.ResidentBuildError, "relative relocation triple"):
            BUILD.read_image(mutant)

    def test_external_absolute_symbol_is_refused(self):
        clang = target_compiler()
        if not clang.is_file(): self.skipTest("S1C33 compiler is required")
        fixture = self.directory / "external.c"
        fixture.write_text('extern unsigned outside; unsigned *resident_entry(void){return &outside;}\n',encoding="ascii")
        target = self.directory / "external.o"
        subprocess.run([str(clang), "--target=s1c33-none-elf", "-Os", "-ffreestanding", "-c",
                        str(fixture), "-o", str(target)],check=True,capture_output=True)
        elf = self.directory / "external.elf"
        subprocess.run([str(clang), "--target=s1c33-none-elf", "-nostdlib",
                        f"-Wl,-T,{ROOT/'device/resident_link.ld'}", "-Wl,--emit-relocs",
                        "-Wl,--defsym=outside=0x02000200", str(target), "-o", str(elf)],
                       check=True,capture_output=True)
        with self.assertRaisesRegex(BUILD.ResidentBuildError, "absolute symbolic"):
            BUILD.read_image(elf)


if __name__ == "__main__":
    unittest.main()
