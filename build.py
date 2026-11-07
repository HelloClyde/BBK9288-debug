"""Build 9288 KF2 development apps; SDK/firmware are local inputs, not bundled."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zlib

ROOT = Path(__file__).resolve().parent

def run(arguments):
    subprocess.run([str(x) for x in arguments], check=True)

def headers(sdk, output):
    source = sdk / "Down_Include"
    if not source.is_dir():
        raise SystemExit(f"SDK missing: {source}")
    for path in source.rglob("*.h"):
        rel = path.relative_to(source)
        data = b"".join(line.replace(b"\\", b"/") if line.lstrip().startswith(b"#include") else line
                        for line in path.read_bytes().splitlines(keepends=True))
        for variant in {rel, Path(*(s.lower() for s in rel.parts)),
                        Path(*(s.lower() for s in rel.parts[:-1]), rel.name)}:
            dest = output / variant
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=Path(r"D:\Downloads\app_env_9288"))
    parser.add_argument("--toolchain", type=Path, default=ROOT.parent / "_tools/s1c33-9288")
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    bindir = args.toolchain.resolve() / "bin"
    clang, objcopy = bindir / "clang.exe", bindir / "llvm-objcopy.exe"
    packer = sdk / "apmk/gen_ap_kf2.exe"
    for item in (clang, objcopy, bindir / "ld.lld.exe", packer, sdk / "apmk/ico1.bin", sdk / "apmk/ico2.bin"):
        if not item.is_file(): raise SystemExit(f"Missing build input: {item}")
    probe = subprocess.run([str(clang), "--target=s1c33-none-elf", "-Os", "-S", "-x", "c", "-", "-o", "-"],
        input="unsigned a(unsigned x){return x;} unsigned d(unsigned a,unsigned b,unsigned c,unsigned d){return d;}",
        text=True, capture_output=True, check=True)
    if not all(re.search(r"ld\.w\s+%r4,\s*%r"+str(r)+r"\b", probe.stdout) for r in (6,9)):
        raise SystemExit("Wrong S1C33 ABI: requires the BBK R6-R9 patch")
    build, dist = ROOT / "build", ROOT / "dist"
    build.mkdir(exist_ok=True); dist.mkdir(exist_ok=True)
    headers(sdk, build / "sdk-headers")
    sources = sorted((ROOT / "device").rglob("*.c")) + sorted((ROOT / "device").rglob("*.h"))
    sources += [ROOT / "device/link.ld", ROOT / "build.py"]
    digest = hashlib.sha256()
    for path in sources:
        digest.update(path.relative_to(ROOT).as_posix().encode()+b"\0"+path.read_bytes())
    image_id = zlib.crc32(digest.digest())
    flags = ["--target=s1c33-none-elf", "-Os", "-ffreestanding", "-fno-builtin", "-fno-jump-tables",
             "-fomit-frame-pointer", "-fno-strict-aliasing", "-DDL_DOWN", "-D_RLS_",
             "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-int-to-pointer-cast", "-Wno-nonportable-include-path",
             "-I", ROOT / "device/compat", "-I", build / "sdk-headers", "-I", ROOT / "device",
             f"-DDEBUG_IMAGE_ID={image_id}u"]
    outputs = []
    for name, title, files in (
        ("9288DebugProbe-v3", "D88PRB3", ["startup.c", "probe.c"]),
        ("9288Debug-dev", "D88DEBUG", ["startup.c", "app.c", "debug_core.c", "usb_cdc.c"]),
    ):
        objects = []
        for file in files:
            obj = build / (name + "-" + Path(file).stem + ".o")
            run([clang, *flags, "-c", ROOT / "device" / file, "-o", obj]); objects.append(obj)
        elf = build / (name + ".elf")
        run([clang, "--target=s1c33-none-elf", "-nostdlib", f"-Wl,-T,{ROOT / 'device/link.ld'}",
             f"-Wl,-Map,{build / (name+'.map')}", *objects, "-o", elf])
        binary = build / (name + ".bin")
        run([objcopy, "-O", "binary", "--gap-fill", "255", elf, binary])
        app = dist / (name + ".exe")
        run([packer, "-module", "9", "-icon1", sdk / "apmk/ico1.bin", "-icon2", sdk / "apmk/ico2.bin",
             "-name", title, "-i", binary, "-o", app])
        sha = hashlib.sha256(app.read_bytes()).hexdigest()
        app.with_suffix(".exe.sha256").write_text(f"{sha}  {app.name}\n", encoding="ascii")
        outputs.append({"file": app.name, "size": app.stat().st_size, "sha256": sha})
    report = {"image_id": f"{image_id:08x}", "source_sha256": digest.hexdigest(),
              "device_mode": "foreground-development", "usb_platform_gate": "closed",
              "hardware_verified": False, "resident_implemented": False,
              "firmware_sha256": "6a2f50e12ba31574c09d714518e9ff3029f01977a4b57fbaa7c0f77bde57d9df",
              "probe_profiles": [
                  {"id":1,"source":"local release kernel.bin","sha256":"6a2f50e12ba31574c09d714518e9ff3029f01977a4b57fbaa7c0f77bde57d9df"},
                  {"id":2,"source":"verified v7 NAND data body","sha256":"13a2977847d105508e43e03a07260b4cc253599066b0248c75948043ada10408","internal_version_marker":"V1.3"}
              ],
              "outputs": outputs}
    (dist / "build.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(report, indent=2))

if __name__ == "__main__": main()
