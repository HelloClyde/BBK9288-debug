"""Independent KF2 container encoder and original, reproducible Debug icons.

The 48-byte little-endian container layout is a file-format interface. This
module does not copy or execute the SDK packer or use its artwork.
"""
from __future__ import annotations

import struct

HEADER = struct.Struct("<IIHH16sIIIII")


def debug_icon(size: int) -> bytes:
    if size not in (16, 40):
        raise ValueError("KF2 Debug icon must be 16 or 40 pixels")
    # Original terminal prompt artwork, drawn on an integer 16x16 grid.
    pixels = []
    for y in range(size):
        for x in range(size):
            gx, gy = x * 16 // size, y * 16 // size
            border = (gx in (1, 14) and 2 <= gy <= 13) or (gy in (2, 13) and 1 <= gx <= 14)
            prompt = (gx, gy) in {(4, 6), (5, 7), (6, 8), (5, 9), (4, 10)}
            underscore = gy == 10 and 8 <= gx <= 11
            pixels.append(0 if border or prompt or underscore else 3)
    packed = bytearray()
    for offset in range(0, len(pixels), 4):
        a, b, c, d = pixels[offset:offset + 4]
        packed.append((a << 6) | (b << 4) | (c << 2) | d)
    return struct.pack("<6H", size, size, 2, 0, 0, 0) + packed


def pack(payload: bytes, title: str, *, icon1: bytes | None = None,
         icon2: bytes | None = None) -> bytes:
    name = title.encode("gbk")
    if not name or len(name) > 15 or b"\0" in name:
        raise ValueError("KF2 title must contain 1..15 GBK bytes")
    icon1 = debug_icon(40) if icon1 is None else icon1
    icon2 = debug_icon(16) if icon2 is None else icon2
    for icon, size in ((icon1, 40), (icon2, 16)):
        if len(icon) != 12 + size * size // 4 or struct.unpack_from("<6H", icon) != (size, size, 2, 0, 0, 0):
            raise ValueError("Invalid KF2 icon")
    code = HEADER.size + len(icon1) + len(icon2)
    total = code + len(payload)
    if not payload or total > 1024 * 1024:
        raise ValueError("9288 KF2 must contain a payload and fit the 1 MiB file limit")
    return HEADER.pack(0x0032464B, 0x19760212, 1, 9, name.ljust(16, b"\0"),
                       code, HEADER.size, len(icon1), len(icon2), total) + icon1 + icon2 + payload


def inspect(data: bytes, expected_title: str = "D88DEBUG") -> dict:
    if len(data) < HEADER.size:
        raise ValueError("Truncated KF2 header")
    magic0, magic1, machine, module, name, code, icons, size1, size2, total = HEADER.unpack_from(data)
    if (magic0, magic1, machine, module, icons, size1, size2, total) != (
            0x0032464B, 0x19760212, 1, 9, 48, 412, 76, len(data)):
        raise ValueError("Invalid 9288 Debug KF2 metadata")
    if name != expected_title.encode("gbk").ljust(16, b"\0") or code != icons + size1 + size2:
        raise ValueError("Invalid 9288 Debug title or payload offset")
    # Re-encoding also validates both icon descriptors and total application size.
    if pack(data[code:], expected_title, icon1=data[icons:icons + size1],
            icon2=data[icons + size1:code]) != data:
        raise ValueError("KF2 container did not round-trip")
    return {"format": "KF2", "machine": machine, "module": module,
            "title": expected_title, "code_offset": code, "bytes": len(data)}
