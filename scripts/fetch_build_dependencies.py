"""Fetch only hash-pinned headers and Windows S1C33 tools; never extract ROMs."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
from urllib.request import urlopen
import zipfile

ROOT = Path(__file__).resolve().parents[1]
URL = "https://github.com/HelloClyde/gamforge/releases/download/v1.5.4/gamforge-1.5.4-full-windows-x64.zip"
SHA256 = "1c51ae2c416bfef265c64a4d0e86f29dfa0dfe5a985a9fe05719787ed730206d"
PREFIXES = {
    "GAMForge/dependencies/sdk9288/Down_Include/": "sdk/Down_Include/",
    "GAMForge/dependencies/toolchain/": "toolchain/",
}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/dependencies")
    parser.add_argument("--archive", type=Path, help="Use an already downloaded, still hash-checked ZIP")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    archive = args.archive or output / "gamforge-1.5.4-full-windows-x64.zip"
    if not archive.is_file():
        if args.archive:
            parser.error("Requested local archive does not exist")
        temporary = archive.with_suffix(".download")
        with urlopen(URL, timeout=120) as response, temporary.open("wb") as stream:
            shutil.copyfileobj(response, stream)
        if digest(temporary) != SHA256:
            raise SystemExit("Pinned dependency archive SHA-256 mismatch")
        temporary.replace(archive)
    if digest(archive) != SHA256:
        raise SystemExit("Pinned dependency archive SHA-256 mismatch")
    files = []
    with zipfile.ZipFile(archive) as zipped:
        for info in zipped.infolist():
            for prefix, local_prefix in PREFIXES.items():
                if not info.filename.startswith(prefix) or info.is_dir():
                    continue
                relative = PurePosixPath(local_prefix + info.filename[len(prefix):])
                if (relative.is_absolute() or ".." in relative.parts or "\\" in str(relative)
                        or any(":" in part or part.rstrip(" .") != part for part in relative.parts)):
                    raise SystemExit("Unsafe dependency archive path")
                if stat.S_ISLNK(info.external_attr >> 16):
                    raise SystemExit("Dependency archive contains an unexpected symlink")
                path = output.joinpath(*relative.parts).resolve()
                if not path.is_relative_to(output):
                    raise SystemExit("Dependency extraction escaped the output directory")
                path.parent.mkdir(parents=True, exist_ok=True)
                data = zipped.read(info)
                path.write_bytes(data)
                files.append({"path": relative.as_posix(), "sha256": hashlib.sha256(data).hexdigest()})
    for relative in ("sdk/Down_Include/Dsys.h", "toolchain/bin/clang.exe",
                     "toolchain/bin/ld.lld.exe", "toolchain/bin/llvm-objcopy.exe",
                     "toolchain/LLVM-LICENSE.TXT", "toolchain/manifest.json"):
        if not (output / relative).is_file():
            raise SystemExit("Missing pinned build input: " + relative)
    report = {"url": URL, "archive_sha256": SHA256,
              "scope": "9288 SDK headers and native Windows S1C33 tools only",
              "roms_extracted": False, "sdk_packer_extracted": False,
              "files": sorted(files, key=lambda item: item["path"])}
    (output / "DEPENDENCIES.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": "PASS", "files": len(files), "archive_sha256": SHA256}))


if __name__ == "__main__":
    main()
