#!/usr/bin/env python3
"""Native ARM64 -> private ARM32 DB bridge tests; public fixture inputs only.

No UCI/real credentials, daemon/service install, cspd/multiapd or real WLAN.
Independent /tmp package; mandatory private namespaces, no host-IPC fallback.
"""
import argparse
import hashlib
import io
import json
import re
import shlex
import struct
import subprocess
import tarfile
from pathlib import Path

from device_workspace import DeviceWorkspace
from bridge_evidence import verify as verify_security
from prepare import verify_existing
from vendor_elf import ELF, audit


def main():
    repo = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="root@192.168.5.1")
    parser.add_argument("--output", type=Path, default=repo / "out")
    args = parser.parse_args()
    if args.host.startswith("-") or not re.fullmatch(r"[a-zA-Z0-9_.@:-]+", args.host):
        parser.error("unsupported SSH destination")
    output = args.output.resolve()
    manifest = verify_existing(output)
    security = verify_security(output / "runtime/lib/libcfapi.so")
    libraries = {n: output / "runtime" / n for n in manifest["files"] if n.startswith("lib/")}
    selected = {"root/" + n: p for n, p in libraries.items()}
    selected.update({"root/bin/db-engine-probe": output / "db-engine-probe",
                     "root/lib/libzte_mesh_shim.so": output / "runtime/lib/libzte_mesh_shim.so",
                     "ns-run": output / "ns-run", "config-bridge-probe": output / "config-bridge-probe"})
    objects = {Path(n).name: ELF.read(p) for n, p in selected.items() if n.startswith("root/")}
    for obj in objects.values():
        obj.require_vendor_abi()
    abi = audit(objects)
    if abi["missing_strong_symbols"] or abi["missing_versions"]:
        parser.error("bridge probe factory ABI mismatch")
    for n in ("ns-run", "config-bridge-probe"):
        header = selected[n].read_bytes()[:64]
        if header[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<H", header, 18)[0] != 183:
            parser.error("bridge/launcher must be native AArch64")
    archive = output / "device-bridge-probe.tar.gz"
    sums = []
    with tarfile.open(archive, "w:gz") as tar:
        for directory in ("root/tmp", "root/dev/shm"):
            item = tarfile.TarInfo(directory)
            item.type, item.mode = tarfile.DIRTYPE, 0o700
            tar.addfile(item)
        for name, path in sorted(selected.items()):
            if path.is_symlink():
                parser.error("refusing package symlink")
            data = path.read_bytes()
            sums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
            item = tarfile.TarInfo(name)
            item.size = len(data)
            item.mode = 0o700 if name in ("ns-run", "config-bridge-probe", "root/bin/db-engine-probe", "root/lib/ld-linux.so.3") else 0o600
            tar.addfile(item, io.BytesIO(data))
        data = "".join(sums).encode()
        item = tarfile.TarInfo("SHA256SUMS")
        item.size, item.mode = len(data), 0o600
        tar.addfile(item, io.BytesIO(data))
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", args.host]
    logs = output / "device-bridge"
    logs.mkdir(exist_ok=True)

    def remote(command, name, timeout=30):
        r = subprocess.run(ssh + [command], capture_output=True, text=True, timeout=timeout)
        (logs / (name + ".stdout")).write_text(r.stdout)
        (logs / (name + ".stderr")).write_text(r.stderr)
        return r

    with DeviceWorkspace(remote, archive, "/tmp/zte-mesh-db.") as workspace:
        directory = workspace.directory
        snapshot = "for p in /sys/class/net/*; do printf '%s\\n' \"${p##*/}\"; done; printf 'HOST_IPC\\n'; for p in /proc/sysvipc/shm /proc/sysvipc/sem; do test -r \"$p\" || exit 1; cat \"$p\"; done"
        before = remote(snapshot, "host-before")
        if before.returncode:
            raise SystemExit("host IPC snapshot unavailable; refusing bridge")
        with archive.open("rb") as stream:
            r = subprocess.run(ssh + [f"tar -xz -C {shlex.quote(directory)}"], stdin=stream, capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit("bridge package upload failed")
        if remote(f"cd {shlex.quote(directory)} && sha256sum -c SHA256SUMS", "checksums").returncode:
            raise SystemExit("bridge package checksum failure")
        result = remote(shlex.join([directory + "/config-bridge-probe", directory + "/root"]), "probe", 300)
        after = remote(snapshot, "host-after")
    unchanged = before.returncode == after.returncode == 0 and before.stdout == after.stdout
    private_count = result.stderr.count("private_ipc=1 private_net=1 readonly_root=1 caps=IPC_OWNER")
    redacted = not any(s in result.stdout + result.stderr for s in (
        "zte-bridge-not-a-real-key", "zte-bridge-public-lab", "zte-shim-lab-not-a-live-key", "PUBLIC-NOT-DEVICE-SERIAL"))
    marker = ("PASS scenarios=16 native_arm64_to_arm32=1 typed_wire=1 profile_readback=1 "
              "other_row_preserved=1 rejected_before_db=1 rollback_verified=1 payload_logged=0 controller_ready=0")
    passed = result.returncode == 0 and marker in result.stdout and private_count == 16 and redacted and unchanged
    report = {"host": args.host, "directory": directory, "directory_removed": workspace.removed,
              "tmp_free_kib_before": workspace.free_kib, "probe_exit": result.returncode,
              "execution": "native-arm64-helper-to-arm32-factory-db", "bridge_test_passed": passed,
              "isolated_scenarios": private_count, "scenarios": 16, "vendor_abi": abi,
              "host_ipc_and_interface_names_unchanged": unchanged, "public_canaries_redacted": redacted,
              "wire_bytes": {"request": 120, "response": 24}, "security_map": security,
              "production_bridge_ready": False, "controller_ready": False,
              "fixture": "public-test-only; inherited pipes; no real configuration/credentials"}
    (output / "device-bridge-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print("Bridge experiment directory:", directory)
    print(result.stdout.strip())
    if not passed:
        # Show only fixed diagnostics; never dump arbitrary raw protocol bytes.
        print("\n".join(l for l in result.stderr.splitlines() if any(s in l for s in (" fail=", " error=", " refuse=", "stop=timeout"))))
    print(f"probe_exit={result.returncode} private_scenarios={private_count} redacted={redacted} host_state_unchanged={unchanged}")
    if not passed:
        raise SystemExit("bridge experiment failed; no host fallback permitted")


if __name__ == "__main__":
    main()
