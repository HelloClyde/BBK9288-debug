"""Build the captured-firmware heap resident and native 9288 launcher."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys
import zlib

from build import headers, run
from scripts.kf2_container import inspect, pack

ROOT=Path(__file__).resolve().parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk",type=Path,default=Path(r"D:\Downloads\app_env_9288"))
    parser.add_argument("--toolchain",type=Path,default=ROOT.parent/"_tools/s1c33-9288")
    args=parser.parse_args()
    sdk=args.sdk.resolve();toolchain=args.toolchain.resolve()
    output=ROOT/"build/service";output.mkdir(parents=True,exist_ok=True)
    dist=ROOT/"dist";dist.mkdir(exist_ok=True)
    headers(sdk,output/"sdk-headers")
    resident_sources=[ROOT/"device"/x for x in ["resident_runtime.c","debug_core.c","usb_cdc.c","usb_platform.c","usb_delay.c","input_jobs.c","power_idle.c","gui_jobs.c","file_native.c","file_service.c","file_runtime.c","app_refresh.c","usb_timer_guard.S"]]
    launcher_sources=[ROOT/"device"/x for x in ["startup.c","launcher.c","resident_loader.c","usb_log.c"]]
    source_files=resident_sources+launcher_sources+sorted((ROOT/"device").rglob("*.h"))+[
        ROOT/"device/link.ld",ROOT/"device/resident_link.ld",ROOT/"build_service.py",ROOT/"tools/build_resident.py"]
    digest=hashlib.sha256()
    for path in sorted(set(source_files)):
        digest.update(path.relative_to(ROOT).as_posix().encode()+b"\0"+path.read_bytes())
    image_id=zlib.crc32(digest.digest())
    include=[output/"sdk-headers",ROOT/"device/compat",ROOT/"device",output]
    definitions=["DL_DOWN","_RLS_",f"DEBUG_IMAGE_ID={image_id}u","D9288_USB_PLATFORM_VERIFIED=1","D9288_USB_TIMER_GUARDED=1"]
    builder=[sys.executable,ROOT/"tools/build_resident.py","--toolchain",toolchain,
             "--output",output/"resident","--header",output/"resident_blob.h"]
    for source in resident_sources: builder.extend(["--source",source])
    for directory in include: builder.extend(["--include",directory])
    for definition in definitions: builder.extend(["--define",definition])
    for symbol in ["resident_control","resident_worker","resident_start","resident_request_stop",
                   "usb_diagnostic","usb_platform_misc_failure","usb_platform_timer_guard","device_descriptor","power_idle_diagnostic",
                   "d88_files_gui_progress","files_diagnostic","gui_hook"]:
        builder.extend(["--export",symbol])
    run(builder)
    clang=toolchain/"bin/clang.exe"
    flags=["--target=s1c33-none-elf","-Os","-ffreestanding","-fno-builtin","-fno-jump-tables",
           "-fomit-frame-pointer","-fno-strict-aliasing","-Wall","-Wextra",
           "-Wno-nonportable-include-path","-Wno-int-to-pointer-cast"]
    for directory in include: flags.extend(["-I",directory])
    for definition in definitions: flags.append("-D"+definition)
    objects=[]
    for source in launcher_sources:
        obj=output/(source.stem+".o")
        run([clang,*flags,"-c",source,"-o",obj]);objects.append(obj)
    elf=output/"9288Debug.elf"
    run([clang,"--target=s1c33-none-elf","-nostdlib",f"-Wl,-T,{ROOT/'device/link.ld'}",
         f"-Wl,-Map,{output/'9288Debug.map'}",*objects,"-o",elf])
    binary=output/"9288Debug.bin"
    run([toolchain/"bin/llvm-objcopy.exe","-O","binary","--gap-fill","255",elf,binary])
    app=dist/"9288Debug.exe"
    packaged=pack(binary.read_bytes(),"D88DEBUG")
    container=inspect(packaged)
    app.write_bytes(packaged)
    sha=hashlib.sha256(app.read_bytes()).hexdigest()
    runtime_header=(ROOT/"device/resident_runtime.h").read_text(encoding="utf-8")
    task_priority=int(re.search(r"^#define D88_TASK_PRIORITY (\d+)u$",runtime_header,re.M)[1])
    app.with_suffix(".exe.sha256").write_text(f"{sha}  {app.name}\n",encoding="ascii")
    result={"file":app.name,"size":app.stat().st_size,"sha256":sha,"image_id":f"{image_id:08x}",
            "container":container,
            "source_sha256":digest.hexdigest(),"mode":"heap-resident-experimental",
            "target":"original-9288-captured-v1.3-and-local-v1.5","hardware_usb_verified":False,
            "firmware_profiles":{"V1.3":"hardware-capture-0215ebc4", "V1.5":"local-release-020f5df4"},
            "v15_services":["usb-cdc","screen","ram-read","scratch","gui-keys","stop"],
            "identity":"BBK9288DEBUG/1 CAPTURED-0215EBC4 EXPERIMENTAL",
            "task_slot":8,"heap_lifetime":"retained until reboot; same-build restart supported",
            "task_priority":task_priority,"native_gui_input":"validated PostMessage jobs; direct matrix-polling games excluded",
            "usb_power_supported":True,
            "file_transfer":"GUI1 native filesystem jobs; token/offset/CRC, staged upload and verified commit; cancellation before USB release",
            "auto_power_off":"native idle timer refreshed while debugger runs; original setting untouched; normal countdown resumes on stop",
            "misc_protected_write_readback":True,
            "usb_clock_powerup":"verified delay with USBSNZ held after each USBCLKEN rising edge",
            "usb_stop_report":"native filesystem D88U01..99.BIN with CRC/readback; captured while the owned worker is paused",
            "usb_windows_compatible_id":"USB\\Class_02&SubClass_02&Prot_01",
            "stop_dialog_waits_for_worker":True,
            "usb_failure_diagnostics":"retained platform gate/register snapshot and Misc readback failure shown by launcher",
            "start_lifecycle":"exit native disk mode; USB cable may remain connected; guarded software detach/attach",
            "stop_lifecycle":"CDC detaches; guarded delay restores native disk service without cable removal"}
    (dist/"service-build.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(result,indent=2))

if __name__=="__main__":main()
