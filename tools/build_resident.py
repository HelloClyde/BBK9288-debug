"""Build and audit a closed S1C33 resident image, then emit a C blob header.

Only image-internal symbolic references are accepted. MMIO and firmware calls
must use explicit numeric addresses or copied runtime table values. The image
may not retain pointers into the 0x02700000 download application.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zlib

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASE = 0x00100000
DEFAULT_VERIFY_BASES = (0x02501230, 0x02634560)
ABSOLUTE_TYPES = {2, 3, 4, 5}
RELATIVE_TYPES = {7, 8, 9}
BOUNDARY_SYMBOLS = {"__resident_start", "__resident_file_end", "__resident_bss_start",
                    "__resident_bss_end", "__resident_end"}


class ResidentBuildError(ValueError):
    pass


@dataclass(frozen=True)
class Relocation:
    offset: int
    target_offset: int
    type: int


@dataclass
class ResidentImage:
    data: bytes
    linked_base: int
    memory_size: int
    alignment: int
    entry_offset: int
    relocations: list[Relocation]
    exports: dict[str, int]
    relocation_types: dict[int, int]


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ResidentBuildError(message)


def _symbol(table, name: str):
    candidates = [s for s in table.iter_symbols() if s.name == name and s["st_shndx"] != "SHN_UNDEF"]
    globals_ = [s for s in candidates if s["st_info"]["bind"] == "STB_GLOBAL"]
    if globals_:
        candidates = globals_
    values = {int(s["st_value"]) for s in candidates}
    _require(len(values) == 1, f"Missing or ambiguous resident symbol: {name}")
    return candidates[0]


def read_image(path: Path, entry: str = "resident_entry", exports=()) -> ResidentImage:
    """Audit a final --emit-relocs ELF and construct its file/BSS model."""
    with Path(path).open("rb") as stream:
        elf = ELFFile(stream)
        _require(elf.elfclass == 32 and elf.little_endian and elf["e_machine"] == "EM_SE_C33",
                 "Resident ELF must be little-endian ELF32 S1C33")
        _require(elf["e_type"] == "ET_EXEC", "Expected a linked ET_EXEC resident ELF")
        symbols = elf.get_section_by_name(".symtab")
        _require(symbols is not None, "ELF has no symbol table")
        start = int(_symbol(symbols, "__resident_start")["st_value"])
        file_end = int(_symbol(symbols, "__resident_file_end")["st_value"])
        bss_start = int(_symbol(symbols, "__resident_bss_start")["st_value"])
        bss_end = int(_symbol(symbols, "__resident_bss_end")["st_value"])
        end = int(_symbol(symbols, "__resident_end")["st_value"])
        _require(0 <= start < file_end <= bss_start <= bss_end == end <= 0xffffffff,
                 "Invalid resident file/BSS boundaries")
        _require(end - start <= 0x001f0000, "Resident exceeds the loader heap budget")
        image = bytearray(file_end - start)
        occupied = bytearray(len(image))
        alignment = 16
        executable = []
        for section in elf.iter_sections():
            if not section["sh_flags"] & 2 or not section["sh_size"]:
                continue
            _require(section.name in {".text", ".rodata", ".data", ".bss"},
                     f"Unexpected allocated section: {section.name}")
            address, size = int(section["sh_addr"]), int(section["sh_size"])
            alignment = max(alignment, int(section["sh_addralign"]))
            _require(start <= address and address + size <= end, "Allocated section outside resident")
            if section["sh_type"] == "SHT_NOBITS":
                _require(bss_start <= address and address + size <= bss_end,
                         "NOBITS section outside resident BSS")
                continue
            _require(section["sh_type"] == "SHT_PROGBITS" and address + size <= file_end,
                     "Unsupported initialized resident section")
            offset = address - start
            _require(not any(occupied[offset:offset + size]), "Overlapping allocated resident sections")
            occupied[offset:offset + size] = b"\x01" * size
            image[offset:offset + size] = section.data()
            if section["sh_flags"] & 4:
                executable.append((address, address + size))
        _require(alignment <= 65536 and alignment & (alignment - 1) == 0 and start % alignment == 0,
                 "Invalid resident section alignment")
        entry_address = int(_symbol(symbols, entry)["st_value"])
        _require(entry_address % 2 == 0 and any(a <= entry_address < b for a, b in executable),
                 "Resident entry must be executable image code")
        _require(int(elf["e_entry"]) == entry_address, "ELF entry does not match requested resident entry")
        export_offsets = {}
        names = list(dict.fromkeys([*exports, "resident_control", "resident_worker"]))
        for name in names:
            matches = [s for s in symbols.iter_symbols() if s.name == name and s["st_shndx"] != "SHN_UNDEF"]
            if not matches and name not in exports:
                continue
            value = int(_symbol(symbols, name)["st_value"])
            _require(start <= value < end, f"Export {name} is outside resident memory")
            export_offsets[name] = value - start

        records = []
        relative = []
        types = Counter()
        for section in elf.iter_sections():
            if section["sh_type"] not in ("SHT_RELA", "SHT_REL"):
                continue
            target_section = elf.get_section(int(section["sh_info"]))
            if not target_section["sh_flags"] & 2:
                continue
            _require(section["sh_type"] == "SHT_RELA", "Only explicit-addend S1C33 RELA is supported")
            relocation_symbols = elf.get_section(int(section["sh_link"]))
            for relocation in section.iter_relocations():
                kind = int(relocation["r_info_type"])
                _require(kind in ABSOLUTE_TYPES | RELATIVE_TYPES,
                         f"Unsupported S1C33 relocation type {kind}")
                types[kind] += 1
                symbol = relocation_symbols.get_symbol(int(relocation["r_info_sym"]))
                index = symbol["st_shndx"]
                _require(index != "SHN_UNDEF", f"Undefined resident reference: {symbol.name}")
                if isinstance(index, int):
                    _require(elf.get_section(index)["sh_flags"] & 2,
                             f"Reference to nonresident section: {symbol.name}")
                else:
                    _require(index == "SHN_ABS" and symbol.name in BOUNDARY_SYMBOLS,
                             f"External/absolute symbolic reference is forbidden: {symbol.name}")
                value = int(symbol["st_value"]) + int(relocation["r_addend"])
                _require(start <= value <= end, f"Reference escapes resident: {symbol.name}")
                offset = int(relocation["r_offset"]) - start
                width = 4 if kind == 2 else 2
                _require(offset % 2 == 0 and 0 <= offset <= len(image) - width,
                         "Relocation patch escapes resident file bytes")
                if kind in RELATIVE_TYPES:
                    _require(value < file_end and any(a <= value < b for a, b in executable),
                             "Relative call/jump must target resident code")
                    relative.append((offset, kind, value - start))
                else:
                    records.append(Relocation(offset, value - start, kind))
        # The compiler emits three extension/branch relocations as a unit.
        relative.sort()
        _require(len(relative) % 3 == 0, "Incomplete relative extension relocation")
        for index in range(0, len(relative), 3):
            group = relative[index:index + 3]
            _require([r[1] for r in group] == [7, 8, 9] and
                     [r[0] for r in group] == list(range(group[0][0], group[0][0] + 6, 2)) and
                     len({r[2] for r in group}) == 1,
                     "Malformed relative relocation triple")
        records.sort(key=lambda r: r.offset)
        last_end = 0
        for record in records:
            _require(record.offset >= last_end, "Overlapping resident relocations")
            last_end = record.offset + (4 if record.type == 2 else 2)
        result = ResidentImage(bytes(image), start, end - start, alignment,
                               entry_address - start, records, export_offsets, dict(types))
        # This additionally checks the expected linked immediate at each patch.
        _require(relocate(result, start) == result.data, "Linked relocation encoding did not round-trip")
        return result


def relocate(image: ResidentImage, base: int) -> bytes:
    """Reference patcher used only for independent-link verification/tests."""
    _require(0 <= base <= 0xffffffff - image.memory_size and base % image.alignment == 0,
             "Invalid resident load base")
    data = bytearray(image.data)
    for record in image.relocations:
        value = base + record.target_offset
        linked = image.linked_base + record.target_offset
        if record.type == 2:
            _require(struct.unpack_from("<I", image.data, record.offset)[0] == linked,
                     "R_S1C33_32 linked value mismatch")
            struct.pack_into("<I", data, record.offset, value)
        else:
            encoded = struct.unpack_from("<H", image.data, record.offset)[0]
            if record.type == 3:
                mask, old, new = 0x1fff, linked >> 19 & 0x1fff, value >> 19 & 0x1fff
            elif record.type == 4:
                mask, old, new = 0x1fff, linked >> 6 & 0x1fff, value >> 6 & 0x1fff
            elif record.type == 5:
                mask, old, new = 0x3f0, (linked & 63) << 4, (value & 63) << 4
            else:
                raise ResidentBuildError(f"Unsupported patch record type {record.type}")
            _require(encoded & mask == old, "S1C33 linked immediate mismatch")
            if record.type != 5:
                _require(encoded & 0xe000 == 0xc000, "S1C33 extension opcode mismatch")
            struct.pack_into("<H", data, record.offset, encoded & ~mask | new)
    return bytes(data)


def _run(arguments) -> None:
    subprocess.run([str(value) for value in arguments], check=True)


def verify_abi(clang: Path) -> None:
    result = subprocess.run([str(clang), "--target=s1c33-none-elf", "-Os", "-S", "-x", "c", "-", "-o", "-"],
        input="unsigned first(unsigned a){return a;} unsigned fourth(unsigned a,unsigned b,unsigned c,unsigned d){return d;}",
        text=True, capture_output=True, check=True)
    _require(all(re.search(r"ld\.w\s+%r4,\s*%r" + str(register) + r"\b", result.stdout)
                 for register in (6, 9)), "S1C33 compiler lacks the BBK9288 R6-R9 ABI patch")


def _link(clang: Path, objects: list[Path], output: Path, base: int, entry: str) -> None:
    _run([clang, "--target=s1c33-none-elf", "-nostdlib",
          f"-Wl,-T,{ROOT / 'device/resident_link.ld'}", f"-Wl,--defsym=RESIDENT_BASE={base}",
          f"-Wl,--entry={entry}", "-Wl,--emit-relocs", f"-Wl,-Map,{output.with_suffix('.map')}",
          *objects, "-o", output])


def emit_header(image: ResidentImage, path: Path, symbol: str = "d88_resident_blob") -> None:
    _require(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol) is not None, "Invalid C blob symbol")
    guard = symbol.upper() + "_GENERATED_H"
    lines = [f"#ifndef {guard}", f"#define {guard}", '#include "resident_loader.h"',
             "/* Generated from an audited --emit-relocs S1C33 ELF. */",
             f"#define D88_RESIDENT_ENTRY_OFFSET 0x{image.entry_offset:x}u",
             f"#define D88_RESIDENT_MEMORY_SIZE 0x{image.memory_size:x}u"]
    for name, offset in image.exports.items():
        lines.append(f"#define D88_{name.upper()}_OFFSET 0x{offset:x}u")
    lines.append(f"static const resident_u8 {symbol}_data[] = {{")
    for offset in range(0, len(image.data), 16):
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in image.data[offset:offset + 16]) + ",")
    lines += ["};", f"static const resident_relocation {symbol}_relocations[] = {{"]
    for record in image.relocations:
        lines.append(f"    {{0x{record.offset:x}u, 0x{record.target_offset:x}u, {record.type}u}},")
    if not image.relocations:
        lines.append("    {0, 0, 0}, /* count is zero */")
    packed = b"".join(struct.pack("<III", r.offset, r.target_offset, r.type) for r in image.relocations)
    lines += ["};", f"static const resident_blob {symbol} = {{",
              "    RESIDENT_BLOB_MAGIC, RESIDENT_BLOB_VERSION,",
              f"    {symbol}_data, 0x{len(image.data):x}u, 0x{image.memory_size:x}u, {image.alignment}u,",
              f"    0x{image.entry_offset:x}u, 0x{image.linked_base:x}u, {symbol}_relocations,",
              f"    {len(image.relocations)}u, 0x{zlib.crc32(image.data):08x}u, 0x{zlib.crc32(packed):08x}u",
              "};", "#endif", ""]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="ascii")


def build_resident(sources, output: Path, toolchain: Path | None = None,
                   include_dirs=(), defines=(), entry="resident_entry", exports=(),
                   compiler_flags=(), base=DEFAULT_BASE, verify_bases=DEFAULT_VERIFY_BASES,
                   header: Path | None = None, symbol="d88_resident_blob") -> ResidentImage:
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    toolchain = Path(toolchain or ROOT.parent / "_tools/s1c33-9288").resolve()
    clang = toolchain / "bin/clang.exe"
    _require(clang.is_file(), f"Missing S1C33 compiler: {clang}")
    verify_abi(clang)
    objects = []
    flags = ["--target=s1c33-none-elf", "-Os", "-ffreestanding", "-fno-builtin",
             "-fno-jump-tables", "-fomit-frame-pointer", "-fno-strict-aliasing",
             "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra",
             "-Wno-int-to-pointer-cast", "-Wno-nonportable-include-path"]
    for include in include_dirs:
        flags += ["-I", str(Path(include).resolve())]
    flags += [f"-D{definition}" for definition in defines]
    flags += list(compiler_flags)
    for index, source in enumerate(sources):
        source = Path(source).resolve()
        _require(source.is_file(), f"Missing resident source: {source}")
        target = output / f"{index:02d}-{source.stem}.o"
        _run([clang, *flags, "-c", source, "-o", target])
        objects.append(target)
    _require(bool(objects), "No resident sources supplied")
    elf_path = output / "resident.elf"
    _link(clang, objects, elf_path, base, entry)
    image = read_image(elf_path, entry, exports)
    _require(len(set(verify_bases)) >= 2, "At least two independent verification bases are required")
    for verify_base in verify_bases:
        alternative = output / f"resident-at-{verify_base:08x}.elf"
        _link(clang, objects, alternative, verify_base, entry)
        independently_linked = read_image(alternative, entry, exports)
        _require(image.memory_size == independently_linked.memory_size and
                 image.entry_offset == independently_linked.entry_offset and
                 image.alignment == independently_linked.alignment and
                 image.relocations == independently_linked.relocations and
                 image.exports == independently_linked.exports,
                 "Resident layout changes when its link base moves")
        _require(relocate(image, verify_base) == independently_linked.data,
                 f"Independent link byte comparison failed at 0x{verify_base:08x}")
    (output / "resident.bin").write_bytes(image.data)
    emit_header(image, header or output / "resident_blob.h", symbol)
    manifest = {"file_bytes": len(image.data), "memory_bytes": image.memory_size,
                "linked_base": base, "alignment": image.alignment, "entry_offset": image.entry_offset,
                "exports": image.exports, "relocation_count": len(image.relocations),
                "input_relocation_types": image.relocation_types,
                "sha256": hashlib.sha256(image.data).hexdigest(),
                "abi": "BBK9288 R6-R9",
                "independent_link_bases": list(verify_bases), "byte_comparison": "passed",
                "physical_resident_lifetime_verified": False}
    (output / "resident.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build/resident")
    parser.add_argument("--toolchain", type=Path, default=ROOT.parent / "_tools/s1c33-9288")
    parser.add_argument("--include", type=Path, action="append", default=[])
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--compiler-flag", action="append", default=[])
    parser.add_argument("--entry", default="resident_entry")
    parser.add_argument("--export", action="append", default=[])
    parser.add_argument("--base", type=lambda value: int(value, 0), default=DEFAULT_BASE)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--symbol", default="d88_resident_blob")
    args = parser.parse_args()
    try:
        image = build_resident(args.source, args.output, args.toolchain, args.include, args.define,
                               args.entry, args.export, args.compiler_flag, args.base,
                               header=args.header, symbol=args.symbol)
    except (ResidentBuildError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Resident build failed: {error}\n")
    print(f"Resident: {len(image.data)} file bytes, {image.memory_size} memory bytes, "
          f"{len(image.relocations)} patches; independent links matched")


if __name__ == "__main__":
    main()
