"""Build a version-reviewed runtime with an independent KF2 packaging adapter."""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.kf2_container import inspect, pack

HISTORICAL_PAYLOAD_SHA256 = "f86c57e384562547e5af756439b3298e08dde9b5ea2957e662e4ad858800c402"
HISTORICAL_IMAGE_ID = "f9dc2295"
RUNTIME_RECORD = ROOT / "docs/runtime-validation.json"


def runtime_record() -> dict:
    return json.loads(RUNTIME_RECORD.read_text(encoding="utf-8"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / "build/dependencies/sdk")
    parser.add_argument("--toolchain", type=Path, default=ROOT / "build/dependencies/toolchain")
    parser.add_argument("--record-candidate", action="store_true",
                        help="Build an unapproved candidate; release builds never use this flag")
    args = parser.parse_args()
    sdk, toolchain = args.sdk.resolve(), args.toolchain.resolve()
    if not (sdk / "Down_Include/Dsys.h").is_file():
        parser.error("Missing SDK headers; run scripts/fetch_build_dependencies.py or pass --sdk")
    build_service = importlib.import_module("build_service")

    def run(arguments):
        # Only the final container operation is replaced. Compile/link/relocation
        # auditing still execute the original builder and its original flags.
        if Path(arguments[0]).name == "gen_ap_kf2.exe":
            values = {str(arguments[i]): str(arguments[i + 1]) for i in range(1, len(arguments), 2)}
            output = Path(values["-o"])
            data = pack(Path(values["-i"]).read_bytes(), values["-name"])
            inspect(data, values["-name"])
            output.write_bytes(data)
        else:
            subprocess.run([str(value) for value in arguments], check=True)

    build_service.run = run
    saved_argv = sys.argv
    try:
        sys.argv = [str(ROOT / "build_service.py"), "--sdk", str(sdk), "--toolchain", str(toolchain)]
        build_service.main()
    finally:
        sys.argv = saved_argv
    app = ROOT / "dist/9288Debug.exe"
    data = app.read_bytes()
    container = inspect(data)
    payload_sha = hashlib.sha256(data[container["code_offset"]:]).hexdigest()
    service = json.loads((ROOT / "dist/service-build.json").read_text(encoding="utf-8"))
    expected = runtime_record() if RUNTIME_RECORD.is_file() else {}
    matched = (expected.get("image_id") == service["image_id"] and
               expected.get("payload_sha256") == payload_sha and
               expected.get("device_sha256") == hashlib.sha256(data).hexdigest() and
               expected.get("runtime_source_sha256") == service["source_sha256"] and
               expected.get("version") == (ROOT / "VERSION").read_text().strip())
    if not matched and not args.record_candidate:
        raise SystemExit("Runtime differs from this version's reviewed candidate record; validate and record the new build")
    report = {"result": "PASS" if matched else "CANDIDATE", "version": (ROOT / "VERSION").read_text().strip(),
              "file": app.name, "sha256": hashlib.sha256(data).hexdigest(),
              "image_id": service["image_id"], "container": container,
              "payload_sha256": payload_sha, "runtime_source_sha256": service["source_sha256"],
              "matches_recorded_runtime": matched,
              "matches_recorded_hardware_payload": payload_sha == HISTORICAL_PAYLOAD_SHA256,
              "historical_hardware_baseline": {"image_id": HISTORICAL_IMAGE_ID,
                                               "payload_sha256": HISTORICAL_PAYLOAD_SHA256},
              "physical_app_refresh_verified": bool(matched and expected.get("physical_app_refresh_verified")),
              "packaging": "independent Python KF2 encoder and original terminal icons",
              "container_hardware_retested": bool(matched and expected.get("container_hardware_retested")),
              "validation_scope": expected.get("validation_scope", "unapproved development candidate") if matched else "unapproved development candidate"}
    (ROOT / "dist/device-build-validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
