#!/usr/bin/env python3
"""Audit the built shim, list the factory libraries, then stop at the DB boundary."""
import argparse
import json
import os
import subprocess
from pathlib import Path

from prepare import verify_existing
from vendor_elf import ELF, audit


def validate(output: Path):
    manifest = verify_existing(output)
    runtime = output / "runtime"
    objects = {Path(name).name: ELF.read(runtime / name) for name in manifest["files"]}
    shim = ELF.read(runtime / "lib/libzte_mesh_shim.so")
    objects["libzte_mesh_shim.so"] = shim
    result = audit(objects)
    if result["missing_strong_symbols"] or result["missing_versions"]:
        raise ValueError(f"shim ABI audit failed: {result}")
    if shim.exports != {"DBShmCliInit", "dbAPIGetView"}:
        raise ValueError(f"unexpected shim exports: {sorted(shim.exports)}")
    return {"audit": result, "shim": shim.summary(), "vendor_library_count": manifest["library_count"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    report = validate(output)
    runtime = output / "runtime"
    logs = output / "smoke"
    logs.mkdir(exist_ok=True)
    prefix = [str(args.qemu), "-L", str(runtime)]
    loader = [str(runtime / "lib/ld-linux.so.3"), "--library-path", str(runtime / "lib")]
    environment = dict(os.environ)
    for key in ("LD_PRELOAD", "LD_LIBRARY_PATH", "ZTE_MESH_SHIM_MODE", "ZTE_MESH_SHIM_ALLOW_TRACE"):
        environment.pop(key, None)
    listed = subprocess.run(prefix + loader + ["--list", str(runtime / "bin/multiapd")], env=environment, capture_output=True, text=True, timeout=15)
    (logs / "loader.stdout").write_text(listed.stdout)
    (logs / "loader.stderr").write_text(listed.stderr)
    report["loader_exit"] = listed.returncode
    if listed.returncode:
        raise SystemExit(f"factory loader failed ({listed.returncode}): {listed.stderr[-1000:]}")
    command = prefix + ["-E", f"LD_PRELOAD={runtime}/lib/libzte_mesh_shim.so", "-E", "LD_BIND_NOW=1", "-E", "ZTE_MESH_SHIM_MODE=probe"] + loader + [str(runtime / "bin/multiapd")]
    probe = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=15)
    (logs / "probe.stdout").write_text(probe.stdout)
    (logs / "probe.stderr").write_text(probe.stderr)
    report.update({"execution": "qemu-user", "probe_exit": probe.returncode, "database_intercepted": "call=DBShmCliInit" in probe.stderr, "controller_ready": False})
    (output / "smoke-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if probe.returncode != 78 or not report["database_intercepted"] or "database-backend-not-implemented" not in probe.stderr:
        raise SystemExit(f"unexpected probe result ({probe.returncode}): {probe.stderr[-1500:]}")
    print("PASS: ARM32 factory libraries load, shim ABI/versions match, DBShmCliInit interposed; controlled stop=78")
    print(probe.stderr.strip())


if __name__ == "__main__":
    main()
