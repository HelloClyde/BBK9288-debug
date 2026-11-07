"""Build the validated device runtime, Windows client and explicit release assets."""
from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from urllib.request import urlopen
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(arguments, **kwargs):
    subprocess.run([str(item) for item in arguments], check=True, cwd=ROOT, **kwargs)


def official_license(url: str, destination: Path) -> dict:
    with urlopen(url, timeout=60) as response:
        data = response.read(256 * 1024)
    if not data or len(data) >= 256 * 1024 or b"copyright" not in data.lower():
        raise ValueError("Unexpected official license content: " + url)
    destination.write_bytes(data)
    return {"source_url": url, "sha256": digest(destination)}


def collect_licenses(destination: Path) -> dict:
    from PyInstaller.utils.hooks.tcl_tk import tcltk_info
    import tkinter as tk

    destination.mkdir(parents=True, exist_ok=False)
    python_license = next((Path(sys.base_prefix) / name for name in
                           ("LICENSE.txt", "LICENSE_PYTHON.txt", "LICENSE")
                           if (Path(sys.base_prefix) / name).is_file()), None)
    if python_license is None:
        raise ValueError("Installed CPython license is missing")
    shutil.copyfile(python_license, destination / "CPython-LICENSE.txt")
    components = {"CPython": {"version": sys.version.split()[0]}}
    for package in ("Pillow", "pyserial", "PyInstaller"):
        distribution = importlib.metadata.distribution(package)
        notices = [path for path in distribution.files or []
                   if path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE"))]
        target = destination / package
        target.mkdir()
        components[package] = {"version": distribution.version}
        for index, path in enumerate(notices):
            shutil.copyfile(distribution.locate_file(path), target / f"{index}-{path.name}")
        if not notices:
            if package != "pyserial" or distribution.version != "3.5":
                raise ValueError("Installed package license is missing: " + package)
            source = ROOT / "packaging/third-party/pyserial-3.5-LICENSE.txt"
            if digest(source) != "f91cb9813de6a5b142b8f7f2dede630b5134160aedaeaf55f4d6a7e2593ca3f3":
                raise ValueError("Pinned pyserial notice SHA-256 mismatch")
            shutil.copyfile(source, target / "LICENSE.txt")
            components[package].update({"source_url": "https://github.com/pyserial/pyserial/blob/v3.5/LICENSE.txt",
                                       "sha256": digest(source)})
    root = tk.Tk()
    root.withdraw()
    versions = {"tcl": str(root.tk.call("info", "patchlevel")),
                "tk": str(root.tk.call("package", "require", "Tk"))}
    root.destroy()
    for component, folder in (("tcl", tcltk_info.tcl_data_dir), ("tk", tcltk_info.tk_data_dir)):
        version = versions[component]
        target = destination / (component + "-license.terms")
        local = Path(folder) / "license.terms"
        components[component] = {"version": version}
        if local.is_file():
            shutil.copyfile(local, target)
        elif component == "tcl" and version == "8.6.12":
            source = ROOT / "packaging/third-party/tcl-8.6.12-license.terms"
            if digest(source) != "c0a69a2bfd757361ec7e6143973b103c90409316b49e9c88db26ad6388e79f16":
                raise ValueError("Pinned Tcl notice SHA-256 mismatch")
            shutil.copyfile(source, target)
            components[component].update({"source_url": "https://github.com/tcltk/tcl/blob/core-8-6-12/license.terms",
                                          "sha256": digest(source)})
        else:
            tag = "core-" + version.replace(".", "-")
            components[component].update(official_license(
                f"https://raw.githubusercontent.com/tcltk/{component}/{tag}/license.terms", target))
    components["files"] = [{"path": p.relative_to(destination).as_posix(), "sha256": digest(p)}
                           for p in sorted(destination.rglob("*")) if p.is_file()]
    (destination / "BUILD-COMPONENTS.json").write_text(json.dumps(components, indent=2) + "\n", encoding="utf-8")
    return components


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / "build/dependencies/sdk")
    parser.add_argument("--toolchain", type=Path, default=ROOT / "build/dependencies/toolchain")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/release")
    parser.add_argument("--tag", help="Require this tag to match VERSION")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("The first release builds the Windows client on Windows")
    version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
    if args.tag and args.tag != "v" + version:
        parser.error("Tag does not match VERSION")
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("Release output must be empty; use a new directory for another build")
    output.mkdir(parents=True, exist_ok=True)
    run([sys.executable, "scripts/build_device.py", "--sdk", args.sdk.resolve(),
         "--toolchain", args.toolchain.resolve()])
    work = ROOT / "build" / ("release-" + version)
    work.mkdir(parents=True, exist_ok=False)
    licenses = work / "licenses"
    components = collect_licenses(licenses)
    client_dist = work / "client-dist"
    run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile",
         "--windowed", "--name", "9288Debug-PC", "--distpath", client_dist,
         "--workpath", work / "pyinstaller", "--specpath", work,
         "--paths", ROOT, "--add-data", str(licenses) + os.pathsep + "licenses",
         "scripts/client_entry.py"])
    client = client_dist / "9288Debug-PC.exe"
    smoke = work / "frozen-gui-smoke.json"
    run([client, "--smoke-test-report", smoke], timeout=45)
    smoke_report = json.loads(smoke.read_text(encoding="utf-8"))
    if smoke_report.get("result") != "PASS" or smoke_report.get("serial_opened") is not False:
        raise ValueError("Frozen GUI smoke test failed")
    device = ROOT / "dist/9288Debug.exe"
    shutil.copyfile(device, output / device.name)
    shutil.copyfile(client, output / client.name)
    archive = output / f"BBK9288-debug-{version}-windows-x64.zip"
    explicit = [(device, "device/系统/程序/9288Debug.exe"), (client, "pc/9288Debug-PC.exe")]
    for name in ("README.md", "LICENSE", "NOTICE.md", "VERSION"):
        explicit.append((ROOT / name, name))
    for path in sorted((ROOT / "docs").rglob("*")):
        if path.is_file() and path.suffix.lower() in (".md", ".png", ".json"):
            explicit.append((path, path.relative_to(ROOT).as_posix()))
    for path in sorted(licenses.rglob("*")):
        if path.is_file():
            explicit.append((path, "licenses/" + path.relative_to(licenses).as_posix()))
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as zipped:
        for path, name in explicit:
            zipped.write(path, "BBK9288-debug/" + name)
        zipped.writestr("BBK9288-debug/MANIFEST.json", json.dumps([
            {"path": name, "bytes": path.stat().st_size, "sha256": digest(path)}
            for path, name in explicit], indent=2, ensure_ascii=False) + "\n")
    with zipfile.ZipFile(archive) as zipped:
        if zipped.testzip() is not None:
            raise ValueError("ZIP CRC validation failed")
        manifest = json.loads(zipped.read("BBK9288-debug/MANIFEST.json"))
        for item in manifest:
            if hashlib.sha256(zipped.read("BBK9288-debug/" + item["path"])).hexdigest() != item["sha256"]:
                raise ValueError("ZIP file manifest mismatch")
    source_sha = os.environ.get("GITHUB_SHA")
    if not source_sha:
        try:
            git_root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], cwd=ROOT, text=True).strip()
            if Path(git_root).resolve() == ROOT:
                source_sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        except subprocess.CalledProcessError:
            source_sha = None
    report = {"result": "PASS", "version": version, "source_sha": source_sha,
              "device": json.loads((ROOT / "dist/device-build-validation.json").read_text()),
              "frozen_gui": smoke_report, "components": components,
              "firmware_or_nand_included": False, "sdk_or_toolchain_included": False,
              "assets": [{"name": p.name, "bytes": p.stat().st_size, "sha256": digest(p)}
                         for p in (output / device.name, output / client.name, archive)]}
    (output / "build-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    hashes = "".join(f"{digest(path)}  {path.name}\n" for path in sorted(output.iterdir()) if path.is_file())
    (output / "SHA256SUMS.txt").write_text(hashes, encoding="ascii")
    print(json.dumps({"result": "PASS", "version": version, "assets": [p.name for p in output.iterdir()]}))


if __name__ == "__main__":
    main()
