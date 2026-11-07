"""Check the intended public file set without collecting local dependencies."""

from pathlib import Path
import re
import subprocess
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN = {".exe", ".dll", ".elf", ".o", ".bin", ".raw", ".flat", ".sav", ".gam", ".res", ".zip", ".log", ".pyc"}
PRIVATE_DIRS = {"reference", "captures", "build", "dist", ".deps", "__pycache__"}


def main():
    result = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT
    ).decode("utf-8")
    names = sorted(set(filter(None, result.split("\0"))))
    if not names:
        raise SystemExit("No public source files found")
    errors = []
    for name in names:
        path = ROOT / name
        if path.suffix.lower() in FORBIDDEN or PRIVATE_DIRS.intersection(Path(name).parts):
            errors.append(f"Private/generated input: {name}")
        if path.is_symlink() or not path.is_file():
            errors.append(f"Invalid source entry: {name}")
            continue
        if path.stat().st_size > 2_000_000:
            errors.append(f"Unexpected large source entry: {name}")
        if path.suffix.lower() == ".md":
            contents = path.read_text(encoding="utf-8")
            for target in re.findall(r"\]\(([^)]+)\)", contents):
                target = target.split("#", 1)[0].strip("<>")
                if not target or re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", target):
                    continue
                resolved = (path.parent / unquote(target)).resolve()
                if not resolved.is_relative_to(ROOT) or not resolved.exists():
                    errors.append(f"Broken local Markdown reference: {name}: {target}")
    if errors:
        raise SystemExit("\n".join(errors))
    print(f"Public-source audit passed: {len(names)} files")


if __name__ == "__main__":
    main()
