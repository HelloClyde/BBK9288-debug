"""Build a local development ZIP; never includes private firmware, NAND, or SDK."""
import hashlib
import json
from pathlib import Path
import zipfile

ROOT=Path(__file__).resolve().parent

def main():
    files=[ROOT/name for name in ["README.md","build.py","build_service.py","build_memory.py","build_layout.py","package.py","inspect_probe.py","inspect_usb_log.py","run_gui.py","requirements.txt","requirements-build.txt","启动9288Debug.cmd"]]
    for directory in ["host","device","memory","memory_layout","tests","docs","tools"]:
        files += [x for x in (ROOT/directory).rglob("*") if x.is_file() and "__pycache__" not in x.parts]
    for name in ["9288Debug-PC.exe","9288Debug.exe","9288Debug.exe.sha256","service-build.json","9288DebugProbe-v3.exe","9288DebugProbe-v3.exe.sha256","validation.json",
                 "9288MemoryProbe.exe","9288MemoryProbe.exe.sha256","memory-build.json","memory-validation.json",
                 "9288LayoutProbe.exe","9288LayoutProbe.exe.sha256","layout-build.json","layout-validation.json"]:
        files.append(ROOT/"dist"/name)
    for path in files:
        if not path.is_file(): raise SystemExit(f"Missing package input: {path}")
    validation=json.loads((ROOT/"dist/validation.json").read_text(encoding="utf-8"))
    service=json.loads((ROOT/"dist/service-build.json").read_text(encoding="utf-8"))
    acceptance=validation.get("physical_usb_debug_acceptance",{})
    memory_validation=json.loads((ROOT/"dist/memory-validation.json").read_text(encoding="utf-8"))
    layout_validation=json.loads((ROOT/"dist/layout-validation.json").read_text(encoding="utf-8"))
    verified=bool(validation.get("physical_hardware_verified") and
                  acceptance.get("build_sha256")==service["sha256"] and
                  hashlib.sha256((ROOT/"dist/9288Debug.exe").read_bytes()).hexdigest()==service["sha256"])
    manifest={"stage":("experimental resident service; physical features verified within recorded coverage"
                       if verified else "experimental resident service; physical CDC acceptance pending"),
              "hardware_verified":verified,
              "physical_verified_features":validation.get("physical_verified_features",[]) if verified else [],
              "physical_unverified_features":validation.get("physical_unverified_features",[]),
              "memory_benchmark_verified":bool(memory_validation.get("physical_memory_benchmark_verified")),
              "additional_program_memory_verified":bool(layout_validation.get("physical_extra_memory_verified")),
              "files":[{"path":x.relative_to(ROOT).as_posix(),"bytes":x.stat().st_size,
                        "sha256":hashlib.sha256(x.read_bytes()).hexdigest()} for x in files]}
    (ROOT/"dist/package.json").write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    files.append(ROOT/"dist/package.json")
    output=ROOT/"9288Debug-development.zip"
    with zipfile.ZipFile(output,"w",compression=zipfile.ZIP_DEFLATED) as archive:
        for path in files: archive.write(path,"9288-debug/"+path.relative_to(ROOT).as_posix())
    sha=hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(".zip.sha256").write_text(f"{sha}  {output.name}\n",encoding="ascii")
    print(f"{output}\n{output.stat().st_size} bytes\nSHA256 {sha}")

if __name__=="__main__": main()
