"""Native allocator smoke on an independent captured-NAND QEMU instance.

No physical serial/disk access and no emulated memory speed claim. Only the
GUI callback pointer and the private emulator's download RAM are injected.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.test_service_native_model import Remote

SNAPSHOT = ["version", "bytes", "result", "start", "end", "data_top",
            "directory_bottom", "record_count", "allocated_records", "free_records",
            "allocated_payload", "free_holes", "largest_hole", "tail_gap", "tail_capacity",
            "max_request", "free_capacity", "metadata_bytes", "directory_crc32"]


def build(out: Path, env: dict) -> dict:
    toolchain = ROOT.parent / "_tools/s1c33-9288/bin"
    clang = toolchain / "clang.exe"
    sources = [ROOT / "device/startup.c", ROOT / "memory/memory_heap.c",
               ROOT / "tests/native_memory_heap_smoke.c",
               ROOT / "tests/native_service_launcher_hook.c"]
    flags = ["--target=s1c33-none-elf", "-Os", "-ffreestanding", "-fno-builtin",
             "-fno-jump-tables", "-fomit-frame-pointer", "-fno-strict-aliasing",
             "-Wall", "-Wextra", "-Werror", "-Wno-nonportable-include-path",
             "-Wno-int-to-pointer-cast", "-DDL_DOWN", "-D_RLS_"]
    for directory in [ROOT / "build/service/sdk-headers", ROOT / "device/compat",
                      ROOT / "memory", ROOT / "device"]:
        flags.extend(["-I", str(directory)])
    objects = []
    for source in sources:
        obj = out / (source.stem + ".o")
        subprocess.run([str(clang), *flags, "-c", str(source), "-o", str(obj)],
                       check=True, env=env, capture_output=True, text=True)
        objects.append(str(obj))
    elf = out / "native-memory-heap-smoke.elf"
    subprocess.run([str(clang), "--target=s1c33-none-elf", "-nostdlib",
                    "-Wl,-T," + str(ROOT / "device/link.ld"),
                    "-Wl,-Map," + str(out / "native-memory-heap-smoke.map"),
                    *objects, "-o", str(elf)], check=True, env=env,
                   capture_output=True, text=True)
    binary = out / "native-memory-heap-smoke.bin"
    subprocess.run([str(toolchain / "llvm-objcopy.exe"), "-O", "binary",
                    "--gap-fill", "255", str(elf), str(binary)], check=True,
                   env=env, capture_output=True, text=True)
    sys.path.insert(0, str(ROOT / "tools"))
    from elftools.elf.elffile import ELFFile
    with elf.open("rb") as handle:
        table = ELFFile(handle).get_section_by_name(".symtab")
        symbols = {symbol.name: int(symbol["st_value"]) for symbol in table.iter_symbols()
                   if symbol.name and symbol["st_shndx"] != "SHN_UNDEF"}
    for name in ["DL_AppMain", "guest_launcher_hook", "guest_hook_status",
                 "guest_hook_result", "native_heap_smoke_report", "native_heap_smoke_report_bytes"]:
        assert name in symbols, name
    inputs = sources + [ROOT / "memory/memory_heap.h", ROOT / "device/link.ld",
                        ROOT / "tools/test_memory_heap_native.py", ROOT / "tools/test_service_native_model.py"]
    manifest = {"sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in inputs},
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "binary_bytes": binary.stat().st_size, "symbols": symbols}
    manifest["source_sha256"] = hashlib.sha256(json.dumps(manifest["sources"], sort_keys=True,
                                                         separators=(",", ":")).encode()).hexdigest()
    (out / "build.json").write_text(json.dumps(manifest, indent=2), encoding="utf8")
    return manifest


def decode(raw: bytes) -> dict:
    words = list(struct.unpack("<" + "I" * (len(raw) // 4), raw))
    cursor = 0

    def take(count):
        nonlocal cursor
        values = words[cursor:cursor + count]
        cursor += count
        return values

    report = dict(zip(["magic", "version", "bytes", "done", "stage", "result", "psr", "gp"], take(8)))
    report["context_before"] = take(4)
    report["context_after"] = take(4)
    report["results"] = take(6)
    report["owns"] = take(6)
    report["fault"], report["canonical_restored"] = take(2)
    report["errno_before"], report["errno_after_probe"], report["errno_after"] = take(3)
    for name in ["raw_before", "raw_after_probe", "raw_allocated", "raw_after"]:
        report[name] = take(5)
    report["before"] = dict(zip(SNAPSHOT, take(19)))
    probe = dict(zip(["version", "bytes", "result", "candidate", "pointer", "boundary_request",
                      "boundary_pointer", "allocated_ok", "boundary_failed", "state_restored",
                      "directory_restored", "errno_before", "errno_after"], take(13)))
    probe["before"] = dict(zip(SNAPSHOT, take(19)))
    probe["after"] = dict(zip(SNAPSHOT, take(19)))
    report["probe"] = probe
    report["allocation"] = dict(zip(["version", "bytes", "result", "address", "allocation_bytes", "active"], take(6)))
    report["allocated"] = dict(zip(SNAPSHOT, take(19)))
    report["after"] = dict(zip(SNAPSHOT, take(19)))
    assert cursor == len(words), (cursor, len(words))
    report["result_signed"] = struct.unpack("<i", struct.pack("<I", report["result"]))[0]
    return report


def validate(report: dict) -> None:
    assert report["magic"] == 0x3848534D and report["version"] == 1 and report["done"] == 1
    assert report["result"] == 0, report["result_signed"]
    assert report["gp"] == 0 and report["psr"] & 0x710 == 0x10
    assert report["context_before"][:2] == [0, 0]
    assert report["context_after"][:2] == [0, 0]
    assert not any(report["results"]) and report["fault"] == 0
    assert report["owns"] == [1, 1, 0, 0, 0, 0]
    assert report["canonical_restored"] == 1 and report["before"] == report["after"]
    assert report["raw_before"] == report["raw_after_probe"] == report["raw_after"]
    assert report["errno_before"] == report["errno_after_probe"] == report["errno_after"]
    probe = report["probe"]
    assert probe["result"] == 0 and probe["allocated_ok"] == probe["boundary_failed"] == 1
    assert probe["state_restored"] == probe["directory_restored"] == 1
    assert probe["before"] == probe["after"] == report["before"]
    assert probe["candidate"] == report["before"]["max_request"]
    assert probe["pointer"] and probe["boundary_request"] == probe["candidate"] + 1
    assert probe["boundary_pointer"] == 0 and probe["errno_before"] == probe["errno_after"]
    allocation = report["allocation"]
    assert allocation["active"] == 1 and allocation["allocation_bytes"] == 1024 * 1024
    assert report["allocated"]["free_capacity"] >= 128 * 1024
    assert report["allocated"]["max_request"] >= 128 * 1024
    assert report["allocated"]["allocated_payload"] == report["before"]["allocated_payload"] + 1024 * 1024


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=12390)
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    out = ROOT / "build/memory-research/allocator-native"
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    manifest = build(out, env)
    if args.build_only:
        print("Native allocator smoke built:", manifest["binary_sha256"])
        return
    source = ROOT / "build/hardware-profile-emulator/native-no-update.raw"
    nand = out / "independent-native-no-update.raw"
    shutil.copyfile(source, nand)
    observations = {"build": manifest, "port": args.port, "nand_source": str(source),
                    "scope": "captured ROS allocator in GUI task; no physical device and no speed claim"}
    log = (out / "qemu.stderr.log").open("w", encoding="utf8")
    process = None
    remote = None
    responses = queue.Queue()
    sequence = 0

    def qmp(command, arguments=None):
        nonlocal sequence
        sequence += 1
        request = {"execute": command, "id": sequence}
        if arguments is not None:
            request["arguments"] = arguments
        process.stdin.write(json.dumps(request) + "\n")
        process.stdin.flush()
        while True:
            reply = responses.get(timeout=15)
            if reply.get("id") == sequence:
                if "error" in reply:
                    raise RuntimeError(reply)
                return reply.get("return")

    def read_qmp():
        for line in process.stdout:
            try:
                responses.put(json.loads(line))
            except ValueError:
                pass

    def native_state():
        state = remote.words(0x02069000, 5)
        assert state[4] <= 256, state
        directory = remote.read(state[1] - state[4] * 8, state[4] * 8)
        return {"heap_state": state, "directory": directory.hex(),
                "errno": remote.words(0x020002B8, 1)[0],
                "ros": remote.words(0x02068660, 8),
                "callback": remote.words(0x0203857C, 1)[0]}

    try:
        process = subprocess.Popen([
            str(ROOT.parent / "bbk9288-qemu/_build/qemu-system-s1c33.exe"),
            "-M", f"bbk9288,usb-connected=off,nand-image={nand.as_posix()},nand-id=0xADDA8015",
            "-cpu", "c33l05,exit-on-halt=off", "-bios",
            str(ROOT.parent / "9288-boot-capture/captures/2026-10-01-v3/BOOT0.BIN"),
            "-display", "none", "-serial", "none", "-monitor", "none", "-qmp", "stdio",
            "-no-shutdown", "-gdb", f"tcp:127.0.0.1:{args.port}"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
            text=True, encoding="utf8", env=env)
        observations["qemu_pid"] = process.pid
        threading.Thread(target=read_qmp, daemon=True).start()
        responses.get(timeout=10)
        qmp("qmp_capabilities")
        time.sleep(8)
        remote = Remote(args.port, lambda: qmp("stop"))
        remote.stop()
        observations["before_hook"] = native_state()
        symbols = manifest["symbols"]
        remote.write(0x02700000, (out / "native-memory-heap-smoke.bin").read_bytes())
        remote.write(0x0203857C, struct.pack("<I", symbols["guest_launcher_hook"]))
        remote.resume()
        qmp("human-monitor-command", {"command-line": "sendkey ret 300"})
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            time.sleep(0.5)
            remote.stop()
            status = remote.words(symbols["guest_hook_status"], 2)
            if status[0] == 2:
                break
            remote.resume()
        remote.stop()
        observations["hook"] = remote.words(symbols["guest_hook_status"], 2)
        observations["after_hook"] = native_state()
        observations["registers"] = qmp("human-monitor-command", {"command-line": "info registers"})
        size = remote.words(symbols["native_heap_smoke_report_bytes"], 1)[0]
        raw = remote.read(symbols["native_heap_smoke_report"], size)
        (out / "report.bin").write_bytes(raw)
        report = decode(raw)
        observations["report"] = report
        assert observations["hook"] == [2, 0], observations["hook"]
        assert observations["after_hook"]["callback"] == 0
        validate(report)
        observations["passed"] = True
        print(json.dumps({"passed": True, "max_request": report["probe"]["candidate"],
                          "allocation": report["allocation"], "canonical_restored": report["canonical_restored"],
                          "directory_crc32": report["before"]["directory_crc32"]}, indent=2), flush=True)
    except Exception as error:
        observations["error"] = repr(error)
        raise
    finally:
        if remote is not None:
            remote.close()
        if process is not None and process.poll() is None:
            try:
                qmp("quit")
                process.wait(timeout=5)
            except Exception:
                process.terminate()
                process.wait(timeout=5)
        observations["qemu_exit_code"] = process.returncode if process is not None else None
        log.close()
        (out / "observations.json").write_text(json.dumps(observations, indent=2), encoding="utf8")


if __name__ == "__main__":
    main()
