"""Explicit, staged physical acceptance for 9288 build 329f05d6.

Run only while the human operator has returned the 9288 to D88DEBUG READY.
This script never opens COM4, sends STOP/ENTER, deletes arbitrary files, or
overwrites a test name not created successfully by this invocation.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import threading
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from host.client import (CAP_FILES, CAP_KEY, CAP_SCREEN, FILE_CANCELLED, FILE_PUT_DATA,
                         Client, FileError, IDENTITY, VID, PID, ports, scan_code)

EXPECTED_IMAGE = 0x329F05D6
EXPECTED_SERVICE_SHA256 = "adc7f3f3d4b5e18adb39a57269181745ef611c081fdf51cc42f3430ae1d74c8d"
EXPECTED_IDENTITY = "BBK9288DEBUG/1 CAPTURED-0215EBC4 EXPERIMENTAL"
TEST_BYTES = 20 * 1024


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def choose_port(requested: str | None, candidates: list[dict]) -> dict:
    """Only enumerated native USB identity; an explicit UART bypass is forbidden."""
    if requested is not None:
        requested = requested.strip().upper()
        if requested == "COM4" or not re.fullmatch(r"COM[1-9][0-9]*", requested):
            raise ValueError("COM4/H1 and non-COM device paths are forbidden")
    allowed = [p for p in candidates if p.get("vid") == 0x1209 and p.get("pid") == 0x9288
               and str(p.get("port", "")).upper() != "COM4"
               and re.fullmatch(r"COM[1-9][0-9]*", str(p.get("port", "")).upper())]
    if requested is not None:
        allowed = [p for p in allowed if str(p["port"]).upper() == requested]
    if len(allowed) != 1:
        raise RuntimeError(f"Expected one enumerated 1209:9288 port excluding COM4; found {allowed!r}")
    return allowed[0]


class AcceptanceClient(Client):
    """Product Client unchanged; retain wire evidence and deterministically request cancellation."""
    def __init__(self, *args, audit: list, **kwargs):
        self.audit = audit
        self.arm_cancel = False
        self.cancel_data_jobs = 0
        self.cancel_trigger = None
        super().__init__(*args, **kwargs)

    def rpc(self, opcode, payload=b""):
        entry = {"opcode": opcode, "request_bytes": len(payload), "started_monotonic": time.monotonic()}
        self.audit.append(entry)
        try:
            result = super().rpc(opcode, payload)
            entry["response_bytes"] = len(result)
            if 11 <= opcode <= 18 and len(result) == 8:
                entry["job_id"], entry["result"] = struct.unpack("<Ii", result)
            elif opcode == 19 and len(result) >= 16:
                entry["job_id"], entry["job_state"], entry["result"], entry["data_bytes"] = struct.unpack_from("<IIiI", result)
            elif opcode == 20 and len(payload) == 4:
                entry["cancel_job_id"], = struct.unpack("<I", payload)
            return result
        except BaseException as error:
            entry["error"] = {"type": type(error).__name__, "message": str(error)}
            raise
        finally:
            entry["finished_monotonic"] = time.monotonic()

    def _wait_file_job(self, opcode, job, cancel_event=None):
        if self.arm_cancel and opcode == FILE_PUT_DATA:
            self.cancel_data_jobs += 1
            if self.cancel_data_jobs == 3:
                if cancel_event is None:
                    raise RuntimeError("Acceptance cancellation event is missing")
                self.arm_cancel = False
                self.cancel_trigger = {"job_id": job, "opcode": opcode, "after_put_data_submits": 3}
                # The third data job has a valid submit acknowledgement. The
                # existing Client sends CANCEL20, then polls this same job.
                cancel_event.set()
        return super()._wait_file_job(opcode, job, cancel_event)


def checked_status(c: Client, required_caps: int) -> dict:
    if c.identity != EXPECTED_IDENTITY:
        raise RuntimeError("9288 strict identity mismatch")
    status = c.status()
    if status["image_id"] != EXPECTED_IMAGE:
        raise RuntimeError(f"Expected image329f05d6, got {status['image_id']:08x}; no test action sent")
    if status["caps"] & required_caps != required_caps:
        raise RuntimeError("Expected capabilities unavailable; no test action sent")
    return status


def verify_download(c, remote, path, expected):
    result = c.get_file(remote, path)
    actual = path.read_bytes()
    if actual != expected:
        raise RuntimeError("Physical file readback differs from the owned test pattern")
    if result["size"] != len(expected) or result["crc32"] != f"{zlib.crc32(expected) & 0xffffffff:08x}":
        raise RuntimeError("Physical file readback CRC32/size differs")
    return {**result, "sha256": sha(actual), "matches_expected": True}


def file_phase(c: AcceptanceClient, output: Path, record: dict) -> None:
    patterns = [bytes((i * 37 + (i >> 3) * 13 + 0x5a) & 255 for i in range(TEST_BYTES))]
    patterns.extend(bytes(value ^ mask for value in patterns[0]) for mask in (0xc3, 0x3c))
    sources = []
    for label, data in zip(("first", "replacement", "cancelled"), patterns):
        source = output / f"source-{label}.bin"
        source.write_bytes(data)
        sources.append(source)
    record["patterns"] = [{"bytes": len(data), "sha256": sha(data),
                           "crc32": f"{zlib.crc32(data) & 0xffffffff:08x}"} for data in patterns]
    record["name_attempts"] = []
    remote = None
    for number in range(1, 10):
        candidate = f"A:\\D88FT{number:02d}.BIN"
        attempt = {"path": candidate, "replace": False}
        record["name_attempts"].append(attempt)
        try:
            attempt["put"] = c.put_file(sources[0], candidate)
            remote = candidate
            record["owned_remote"] = remote
            record["ownership_basis"] = "This invocation's successful default non-replacing PUT commit"
            break
        except FileError as error:
            attempt["result"] = error.result
            if error.result != -705:
                raise
            attempt["existing_unknown_file_preserved"] = True
    if remote is None:
        raise RuntimeError("D88FT01..09 all exist; no unknown target was overwritten")
    record["first_get"] = verify_download(c, remote, output / "first-readback.bin", patterns[0])
    # Replacing is permitted only after this invocation has created the name.
    record["replacement_put"] = c.put_file(sources[1], remote, replace=True)
    record["replacement_get"] = verify_download(c, remote, output / "replacement-readback.bin", patterns[1])
    c.arm_cancel = True
    event = threading.Event()
    cancel_start = len(c.audit)
    try:
        c.put_file(sources[2], remote, replace=True, cancel_event=event)
    except FileError as error:
        record["cancel_result"] = {"result": error.result, "job_id": error.job_id,
                                   "recovery_data_hex": error.payload.hex()}
        if error.result != FILE_CANCELLED:
            raise
    else:
        raise RuntimeError("Early data cancellation unexpectedly committed")
    record["cancel_trigger"] = c.cancel_trigger
    cancel_calls = [entry for entry in c.audit[cancel_start:] if entry["opcode"] == 20]
    if not c.cancel_trigger or len(cancel_calls) != 1:
        raise RuntimeError("The acceptance run did not issue exactly one CancelJob")
    cancel_job = cancel_calls[0]["cancel_job_id"]
    if cancel_job != c.cancel_trigger["job_id"] or not any(
        entry["opcode"] == 19 and entry.get("job_id") == cancel_job and entry.get("job_state") == 3
        for entry in c.audit[cancel_start:]
    ):
        raise RuntimeError("Cancellation did not poll the same accepted job to DONE")
    record["cancelled_get"] = verify_download(c, remote, output / "cancelled-readback.bin", patterns[1])
    record["cancel_preserved_owned_previous_content"] = True
    record["remote_test_file_retained"] = remote
    record["arbitrary_delete_sent"] = False


def key_phase(c: AcceptanceClient, output: Path, record: dict, settle: float) -> None:
    record["screenshots"] = {"before": c.screenshot(output / "before.png")}
    c.press(scan_code("LEFT"))
    record["left_accepted"] = True
    time.sleep(settle)
    record["screenshots"]["left"] = c.screenshot(output / "left.png")
    record["visual_restoration_requires_review"] = True
    record["right_accepted"] = False
    if record["screenshots"]["before"]["raw_sha256"] == record["screenshots"]["left"]["raw_sha256"]:
        record["stopped_after_unchanged_left"] = True
        record["note"] = "LEFT made no visible change; no blind RIGHT sent. Operator review required."
        return
    c.press(scan_code("RIGHT"))
    record["right_accepted"] = True
    time.sleep(settle)
    record["screenshots"]["after_right"] = c.screenshot(output / "after-right.png")
    from PIL import Image
    with Image.open(output / "before.png") as image:
        width, height = image.size
        before = image.convert("L").tobytes()
    record["pixel_comparisons"] = {}
    for name, filename in (("left", "left.png"), ("after_right", "after-right.png")):
        with Image.open(output / filename) as image:
            if image.size != (width, height):
                raise RuntimeError("Key screenshots changed dimensions")
            pixels = image.convert("L").tobytes()
        top = height // 6
        record["pixel_comparisons"][name] = {
            "changed_pixels_from_before": sum(a != b for a, b in zip(before, pixels)),
            "roi": [0, top, width, height],
            "roi_changed_pixels_from_before": sum(a != b for a, b in zip(before[top*width:], pixels[top*width:])),
            "raw_crc32": f"{zlib.crc32(pixels) & 0xffffffff:08x}"}
    record["enter_sent"] = False


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--phase", required=True, choices=("files-only", "keys-only"))
    p.add_argument("--output", required=True, type=Path, help="Fresh captures directory; acceptance.json must not exist")
    p.add_argument("--port", help="An enumerated 1209:9288 USB COM port; COM4 is always forbidden")
    p.add_argument("--timeout", type=float, default=5.0)
    p.add_argument("--settle", type=float, default=0.5)
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    if not 0.05 <= args.timeout <= 60 or not 0 <= args.settle <= 5:
        raise ValueError("timeout/settle out of bounds")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = output / "acceptance.json"
    if evidence.exists():
        raise ValueError("Refusing to overwrite an earlier acceptance report")
    record = {"phase": args.phase, "created_utc": datetime.now(timezone.utc).isoformat(),
              "expected_image_id": f"{EXPECTED_IMAGE:08x}", "expected_service_sha256": EXPECTED_SERVICE_SHA256,
              "stop_sent": False, "enter_sent": False, "arbitrary_delete_sent": False,
              "com4_forbidden": True, "rpc_audit": [], "physical_passed": False,
              "timing_scope": "USB operation timestamps are evidence only, never RAM bandwidth measurements"}
    try:
        if (VID, PID) != (0x1209, 0x9288) or IDENTITY.decode("ascii") != EXPECTED_IDENTITY:
            raise RuntimeError("Host identity constants do not match the acceptance target")
        meta = json.loads((ROOT / "dist/service-build.json").read_text(encoding="utf-8"))
        binary = ROOT / "dist/9288Debug.exe"
        if (int(meta["image_id"], 16) != EXPECTED_IMAGE or meta["sha256"] != EXPECTED_SERVICE_SHA256 or
            sha(binary.read_bytes()) != EXPECTED_SERVICE_SHA256):
            raise RuntimeError("Local service build does not match fixed acceptance image329f05d6")
        record["local_build"] = meta
        record["host_sources"] = {name: sha((ROOT / name).read_bytes()) for name in ("host/client.py", "tools/accept_files_hardware.py")}
        candidates = ports()  # Enumerates only; it does not open a port.
        record["enumerated_candidates"] = candidates
        selected = choose_port(args.port, candidates)
        record["selected_port"] = selected
        required = CAP_FILES if args.phase == "files-only" else CAP_KEY | CAP_SCREEN
        with AcceptanceClient(selected["port"], timeout=args.timeout, audit=record["rpc_audit"]) as c:
            record["identity"] = c.identity
            record["status_before"] = checked_status(c, required)
            if args.phase == "files-only":
                file_phase(c, output, record)
            else:
                key_phase(c, output, record, args.settle)
            record["status_after"] = checked_status(c, required)
            if any(entry["opcode"] == 8 for entry in c.audit):
                raise RuntimeError("Unexpected STOP opcode in audit")
            record["service_left_running"] = True
            record["physical_passed"] = args.phase == "files-only"
            record["key_sequence_completed"] = args.phase == "keys-only" and record.get("right_accepted", False)
    except BaseException as error:
        record["failure"] = {"type": type(error).__name__, "message": str(error),
                             "outcome_unknown": not isinstance(error, FileError) or error.result == -714}
        raise
    finally:
        record["finished_utc"] = datetime.now(timezone.utc).isoformat()
        evidence.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"phase": args.phase, "evidence": str(evidence),
                      "physical_files_passed": record["physical_passed"],
                      "visual_review_needed": record.get("visual_restoration_requires_review", False)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
