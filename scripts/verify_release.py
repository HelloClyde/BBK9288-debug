"""Check exact release assets, executable identity, ZIP members and checksums."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.kf2_container import inspect
from scripts.build_device import runtime_record


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def verify(directory: Path, version: str, source_sha: str | None = None) -> dict:
    archive_name = f"BBK9288-debug-{version}-windows-x64.zip"
    expected = {"9288Debug.exe", "9288Debug-PC.exe", archive_name,
                "build-report.json", "SHA256SUMS.txt"}
    require({p.name for p in directory.iterdir()} == expected, "Unexpected or missing release files")
    entries = {}
    for line in (directory / "SHA256SUMS.txt").read_text(encoding="ascii").splitlines():
        digest, name = line.split("  ", 1)
        require(name in expected - {"SHA256SUMS.txt"} and name not in entries,
                "Invalid checksum file entry")
        entries[name] = digest
    require(set(entries) == expected - {"SHA256SUMS.txt"}, "Incomplete checksums")
    for name, digest in entries.items():
        require(hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest,
                "Asset SHA-256 mismatch: " + name)
    expected_runtime = runtime_record()
    require(expected_runtime.get("version") == version, "Wrong runtime record version")
    device = (directory / "9288Debug.exe").read_bytes()
    container = inspect(device)
    require(hashlib.sha256(device[container["code_offset"]:]).hexdigest() == expected_runtime["payload_sha256"],
            "Actual released payload differs from this version's reviewed runtime")
    report = json.loads((directory / "build-report.json").read_text(encoding="utf-8"))
    require(report.get("result") == "PASS" and report.get("version") == version, "Invalid build result/version")
    if source_sha:
        require(report.get("source_sha") == source_sha, "Wrong release source commit")
    require(report.get("device", {}).get("result") == "PASS", "Unapproved runtime candidate")
    require(expected_runtime.get("device_sha256") == entries["9288Debug.exe"], "Wrong recorded device executable")
    require(report.get("device", {}).get("sha256") == entries["9288Debug.exe"], "Wrong device evidence")
    require(report["device"].get("image_id") == expected_runtime["image_id"], "Wrong runtime image identity")
    require(report["device"].get("matches_recorded_runtime") is True, "Missing runtime byte comparison")
    require(report["device"].get("runtime_source_sha256") == expected_runtime["runtime_source_sha256"], "Wrong runtime source evidence")
    for key in ("physical_app_refresh_verified", "container_hardware_retested"):
        require(report["device"].get(key) is bool(expected_runtime.get(key)), "Wrong physical verification scope: " + key)
    require(report.get("frozen_gui", {}).get("result") == "PASS" and
            report["frozen_gui"].get("serial_opened") is False, "Missing device-free frozen GUI test")
    require(report.get("firmware_or_nand_included") is False and
            report.get("sdk_or_toolchain_included") is False, "Unexpected dependency redistribution")
    with zipfile.ZipFile(directory / archive_name) as zipped:
        require(zipped.testzip() is None, "ZIP CRC failure")
        names = zipped.namelist()
        require(len(names) == len(set(names)), "Duplicate ZIP members")
        for name in names:
            path = PurePosixPath(name)
            require(not path.is_absolute() and ".." not in path.parts and "\\" not in name,
                    "Unsafe ZIP member")
            require(name.startswith("BBK9288-debug/"), "Unexpected ZIP root")
        prefix = "BBK9288-debug/"
        manifest = json.loads(zipped.read(prefix + "MANIFEST.json"))
        require(set(names) == {prefix + item["path"] for item in manifest} | {prefix + "MANIFEST.json"},
                "ZIP manifest does not cover every file")
        for item in manifest:
            data = zipped.read(prefix + item["path"])
            require(len(data) == item["bytes"] and hashlib.sha256(data).hexdigest() == item["sha256"],
                    "ZIP manifest checksum failure")
        require(zipped.read(prefix + "device/系统/程序/9288Debug.exe") == device, "Wrong packaged device EXE")
        require(zipped.read(prefix + "pc/9288Debug-PC.exe") == (directory / "9288Debug-PC.exe").read_bytes(),
                "Wrong packaged Windows client")
        require(any(name.startswith(prefix + "licenses/pyserial/") for name in names), "Missing pyserial notices")
        for name in ("CPython-LICENSE.txt", "tcl-license.terms", "tk-license.terms", "BUILD-COMPONENTS.json"):
            require(prefix + "licenses/" + name in names, "Missing dependency notice: " + name)
    return {"result": "PASS", "version": version, "source_sha": report.get("source_sha"),
            "assets": sorted(expected), "device": container}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--tag")
    parser.add_argument("--source-sha")
    args = parser.parse_args()
    version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
    if args.tag and args.tag != "v" + version:
        parser.error("Tag does not match VERSION")
    print(json.dumps(verify(args.directory, version, args.source_sha)))


if __name__ == "__main__":
    main()
