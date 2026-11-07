"""Decode a 1024-byte DBG88Pxx.BIN from the original BBK 9288."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

PROFILES = {
    (0x021806EC,0x02180058,0x0217FFE4,0x0217FED4,0x0200021C,0x0217FF40):
        (1,"local-release-020f5df4"),
    (0x021E81E8,0x021E7B54,0x021E7AE0,0x021E79D0,0x0200021C,0x021E7A3C):
        (2,"hardware-capture-0215ebc4 (internal V1.3)"),
}

def inspect(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) != 1024:
        raise ValueError(f"Probe size {len(data)}, expected exactly 1024")
    w = struct.unpack("<256I", data)
    if w[0] != 0x50383844 or w[1] not in (1, 2, 3) or w[2] != 1024:
        raise ValueError("Wrong probe magic/version/length")
    recognized_profile = PROFILES.get(w[4:10])
    # v1/v2 only knew the local release, so preserve their original decision.
    profile_match = w[4:9] == (0x021806EC,0x02180058,0x0217FFE4,0x0217FED4,0x0200021C)
    if w[1] == 3:
        profile_match = bool(w[3] & 1)
        if profile_match and (not recognized_profile or w[11] != recognized_profile[0] or w[29] != 7):
            raise ValueError("v3 profile/signature metadata does not match tables")
    if w[1] == 1 and not profile_match:
        raise ValueError("Report does not match the known 9288 V1.5 profile")
    if w[1] >= 2:
        if zlib.crc32(data[:1020]) & 0xffffffff != w[255]:
            raise ValueError("Probe CRC32 mismatch")
        if bool(w[3] & 1) != profile_match or w[28] not in (1, 2):
            raise ValueError("Invalid profile/stage metadata")
        if w[28] == 2 and not profile_match:
            raise ValueError("Unknown profile must not have version-specific data")
    full = w[1] == 1 or w[28] == 2
    if full and w[25:28] != (1,8,72):
        raise ValueError("Missing task layout metadata; use the current probe build")
    tasks = []
    for i in range(8 if full else 0):
        offset = 128+i*72
        tasks.append({"id":i+1,"initial_stack":f"0x{struct.unpack_from('<I',data,offset+20)[0]:08x}",
                      "entry":f"0x{struct.unpack_from('<I',data,offset+24)[0]:08x}",
                      "priority_index":data[offset+61],"priority":data[offset+61]+1,"state":data[offset+63],
                      "raw":data[offset:offset+72].hex()})
    return {"file":str(path.resolve()),"sha256":hashlib.sha256(data).hexdigest(),
            "version":w[1],"hardware_usb_verified":False,
            "profile_match":profile_match,"report_kind":"full" if full else "table-only",
            "recognized_profile":recognized_profile[1] if recognized_profile else None,
            "profile_id":w[11] if w[1]>=3 else None,
            "function_signature_mask":w[29] if w[1]>=3 else None,
            "tables":{name:f"0x{w[4+i]:08x}" for i,name in enumerate(["ros33","gui","fs","audio","crtl","dict","reserved"])},
            "tcb_base":f"0x{w[24]:08x}" if full else None,"tasks":tasks,
            "fs_functions":{name:f"0x{w[12+i]:08x}" for i,name in enumerate(["fopen","fclose","fread","fwrite"])} if w[1]>=2 else {},
            "profile_addresses":{name:f"0x{w[224+i]:08x}" for i,name in enumerate(["tcb_pointer","heap_state","usb_desktop","usb_phase1","usb_phase2","usb_io_base","usb_flags"])} if full and w[1]>=3 else {},
            "heap_words":[f"0x{x:08x}" for x in w[16:24]] if full else [],
            "timer3b_isr":f"0x{w[232]:08x}" if full and w[1]>=3 else None,
            "kernel_entry":f"0x{w[238]:08x}" if full and w[1]>=3 else None,
            "lcd_modes":list(w[241:243]) if full and w[1]>=3 else None,
            "bcu":{f"0x{a:08x}":f"0x{w[176+i]:08x}" for i,a in enumerate(
                [0x48120,0x48124,0x48128,0x4812c,0x48130,0x4813c])} if full else {},
            "registers":{f"0x{a:08x}":f"0x{w[182+i]:02x}" for i,a in enumerate(
                [0x300f20,0x300f35,0x402a6,0x402a9,0x300022,0x40273,0x40283])} if full else {},
            "native_usb_state":{"desktop":w[192],"phase":w[193],"phase2":w[194],
                                "io_base":f"0x{w[195]:08x}","gui_flags":list(w[196:204])} if full else None,
            "note":"Read-only inventory; it does not prove USB ownership, enumeration, or background lifetime."}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("file",type=Path)
    p.add_argument("--output",type=Path)
    args=p.parse_args()
    try:
        result=inspect(args.file)
        text=json.dumps(result,ensure_ascii=False,indent=2)
        if args.output:
            with args.output.open("x",encoding="utf-8") as output: output.write(text+"\n")
        print(text)
    except (ValueError,OSError) as e: p.error(str(e))

if __name__=="__main__": main()
