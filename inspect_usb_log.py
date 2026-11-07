"""Decode a fixed 1024-byte D88Uxx.BIN USB diagnostic log from original 9288."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

MAGIC, VERSION, BYTES, MAX_REGISTERS = 0x55383844, 1, 1024, 88
CONTROL_FIELDS = (
    "magic", "version", "structure_bytes", "image_id", "self_base",
    "allocation_bytes", "allocation_raw", "task_id", "task_created", "state",
    "last_error", "stop_requested", "starts", "polls", "worker_address",
    "start_address", "stop_address",
)
DIAG_FIELDS = (
    "version", "phase", "result", "failure_mask", "bcu_hi", "bcu_lo", "misc_f20",
    "clock_f35", "usb_irq", "timer_irq", "usb_valid", "revision", "control",
    "status", "xcvr", "main_status", "sie_status", "sie_enable", "test",
    "ep0_status", "ep0_control", "ep0_in", "ep0_out", "ep0_max", "address",
    "setup_count", "poll_count",
)
STATE_NAMES = {0: "NEW", 1: "STARTING", 2: "RUNNING", 3: "STOPPING",
               4: "WAIT_USB", 5: "STOPPED", 6: "FAILED"}
FAILURE_BITS = (
    "BCU_HI", "BCU_LO", "CLOCK_MISSING", "SNOOZE", "MODULE_CLOCK", "USB_IRQ",
    "TIMER_IRQ", "REVISION", "ACTIVE", "PULLUP", "DETACH_WAIT", "BUSY", "UNVERIFIED",
)


def signed(word: int) -> int:
    return word if word < 0x80000000 else word - 0x100000000


def decode(data: bytes) -> dict:
    if len(data) != BYTES:
        raise ValueError(f"USB log size {len(data)}, expected exactly {BYTES}")
    words = struct.unpack("<256I", data)
    if words[0] != MAGIC or words[1] != VERSION or words[2] != BYTES:
        raise ValueError("Wrong USB log magic/version/length")
    if words[5] > MAX_REGISTERS:
        raise ValueError(f"USB register_count {words[5]} exceeds {MAX_REGISTERS}")
    if zlib.crc32(data[:1020]) & 0xFFFFFFFF != words[255]:
        raise ValueError("USB log CRC32 mismatch")
    control = dict(zip(CONTROL_FIELDS, words[8:25]))
    diagnostic = dict(zip(DIAG_FIELDS, words[32:59]))
    registers = [{"address": f"0x{words[64 + i * 2]:08x}",
                  "value": f"0x{words[65 + i * 2]:08x}"}
                 for i in range(words[5])]
    return {
        "sha256": hashlib.sha256(data).hexdigest(),
        "header": dict(zip(("magic", "version", "bytes", "image_id", "flags",
                            "register_count", "capture_kind", "reserved"), words[:8])),
        "image_id": f"{words[3]:08x}", "crc32": "passed",
        "control": control,
        "control_state": STATE_NAMES.get(control["state"], "UNKNOWN"),
        "control_last_error_signed": signed(control["last_error"]),
        "diagnostic": diagnostic,
        "diagnostic_result_signed": signed(diagnostic["result"]),
        "diagnostic_failures": [name for bit, name in enumerate(FAILURE_BITS)
                                if diagnostic["failure_mask"] & (1 << bit)],
        "registers": registers,
        "register_map": {row["address"]: row["value"] for row in registers},
        "control_padding": list(words[25:32]),
        "diagnostic_padding": list(words[59:64]),
        "reserved_words": list(words[240:255]),
        "note": "Diagnostic capture; this file does not establish physical USB enumeration or recovery.",
    }


def inspect(path: Path) -> dict:
    return {"file": str(path.resolve()), **decode(path.read_bytes())}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        result = inspect(args.file)
        rendered = json.dumps(result, ensure_ascii=False, indent=2)
        if args.output:
            with args.output.open("x", encoding="utf-8") as output:
                output.write(rendered + "\n")
        print(rendered)
        return 0
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
