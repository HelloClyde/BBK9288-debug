"""Configurable test compilers; proprietary firmware is never required by CI."""

import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def native_gcc():
    configured = os.environ.get("D88_GCC")
    if configured:
        compiler = shutil.which(configured) or configured
        if not Path(compiler).is_file():
            raise RuntimeError("D88_GCC does not identify an installed compiler")
        return compiler
    found = shutil.which("gcc")
    if found:
        return found
    # Preserve the original local setup as a final Windows-only fallback.
    legacy = Path("C:/msys64/ucrt64/bin/gcc.exe")
    return str(legacy) if os.name == "nt" and legacy.is_file() else ""


def target_toolchain():
    configured = os.environ.get("D88_TOOLCHAIN")
    if configured:
        return Path(configured).resolve()
    local = ROOT / ".deps/toolchain"
    if any((local / "bin" / name).is_file() for name in ("clang.exe", "clang")):
        return local
    return ROOT.parent / "_tools/s1c33-9288"


def target_compiler():
    root = target_toolchain()
    candidates = [root / "bin" / name for name in ("clang.exe", "clang")]
    found = next((path for path in candidates if path.is_file()), candidates[0])
    if os.environ.get("D88_TOOLCHAIN") and not found.is_file():
        raise RuntimeError("D88_TOOLCHAIN must contain bin/clang.exe or bin/clang")
    return found
