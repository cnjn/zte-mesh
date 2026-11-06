#!/usr/bin/env python3
"""Deploy a checksummed /tmp-only probe over SSH. Never starts a Controller.

Native ARM32 execution is checked first. --emulate explicitly permits a second
QEMU-user probe if execve reports ENOEXEC; it does not hide a native failure.
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
from prepare import verify_existing
from smoke import validate


def main():
    repo = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="root@192.168.5.1")
    parser.add_argument("--output", type=Path, default=repo / "out")
    parser.add_argument("--emulate", action="store_true")
    args = parser.parse_args()
    if args.host.startswith("-") or not re.fullmatch(r"[a-zA-Z0-9_.@:-]+", args.host):
        parser.error("unsupported SSH destination")
    output = args.output.resolve()
    report = validate(output)
    runtime = output / "runtime"
    manifest = verify_existing(output)
    selected = {f"runtime/{name}": runtime / name for name in manifest["files"]}
    selected["runtime/lib/libzte_mesh_shim.so"] = runtime / "lib/libzte_mesh_shim.so"
    for name in ("exec-check", "qemu-arm-static") if args.emulate else ("exec-check",):
        path = output / name
        header = path.read_bytes()[:64]
        if header[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<H", header, 18)[0] != 183:
            parser.error(f"{name} must be an AArch64 Linux executable")
        selected[name] = path
    sums = []
    archive = output / "device-probe.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for name, path in sorted(selected.items()):
            if path.is_symlink():
                parser.error(f"refusing symlink in probe bundle: {path}")
            data = path.read_bytes()
            sums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            entry.mode = 0o700 if name in ("exec-check", "qemu-arm-static") or name.endswith(("/multiapd", "/ld-linux.so.3")) else 0o600
            tar.addfile(entry, io.BytesIO(data))
        data = "".join(sums).encode()
        entry = tarfile.TarInfo("SHA256SUMS")
        entry.size, entry.mode = len(data), 0o600
        tar.addfile(entry, io.BytesIO(data))
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", args.host]
    logs = output / "device"
    logs.mkdir(exist_ok=True)

    def remote(command, label):
        result = subprocess.run(ssh + [command], capture_output=True, text=True, timeout=25)
        (logs / f"{label}.stdout").write_text(result.stdout)
        (logs / f"{label}.stderr").write_text(result.stderr)
        return result

    with DeviceWorkspace(remote, archive, "/tmp/zte-mesh-probe.") as workspace:
        directory = workspace.directory
        quote = shlex.quote
        before = remote("for p in /sys/class/net/*; do printf '%s\\n' \"${p##*/}\"; done", "interfaces-before")
        with archive.open("rb") as stream:
            uploaded = subprocess.run(ssh + [f"tar -xz -C {quote(directory)}"], stdin=stream, capture_output=True, text=True, timeout=120)
        if uploaded.returncode:
            raise SystemExit(f"probe upload failed: {uploaded.stderr[-1000:]}")
        checked = remote(f"cd {quote(directory)} && sha256sum -c SHA256SUMS", "checksums")
        if checked.returncode:
            raise SystemExit("device checksum validation failed")
        root = directory + "/runtime"
        loader = [root + "/lib/ld-linux.so.3", "--library-path", root + "/lib"]
        program = root + "/bin/multiapd"
        native = remote(shlex.join([directory + "/exec-check", *loader, "--list", program]), "native-loader")
        report.update({"host": args.host, "directory": directory, "native_loader_exit": native.returncode,
                       "native_arm32_supported": native.returncode == 0, "controller_ready": False})
        failure = None
        if native.returncode == 0:
            listed = native
            command = " ".join([f"LD_PRELOAD={quote(root + '/lib/libzte_mesh_shim.so')}", "LD_BIND_NOW=1", "ZTE_MESH_SHIM_MODE=probe", shlex.join([directory + "/exec-check", *loader, program])])
            probe = remote(command, "native-probe")
            report["execution"] = "native-arm32"
        elif native.returncode == 126 and "execve_errno=8" in native.stderr and args.emulate:
            prefix = [directory + "/exec-check", directory + "/qemu-arm-static", "-L", root]
            listed = remote(shlex.join([*prefix, *loader, "--list", program]), "emulated-loader")
            command = [*prefix, "-E", f"LD_PRELOAD={root}/lib/libzte_mesh_shim.so", "-E", "LD_BIND_NOW=1", "-E", "ZTE_MESH_SHIM_MODE=probe", *loader, program]
            probe = remote(shlex.join(command), "emulated-probe")
            report["execution"] = "qemu-user-on-device"
        else:
            listed = native
            probe = None
            failure = "native ARM32 loading failed; see native-loader.stderr (no automatic emulation)"
        if probe is not None:
            report.update({"loader_exit": listed.returncode, "probe_exit": probe.returncode,
                           "database_intercepted": "call=DBShmCliInit" in probe.stderr})
            if listed.returncode or probe.returncode != 78 or not report["database_intercepted"] or "database-backend-not-implemented" not in probe.stderr:
                failure = "loader/interception probe did not meet acceptance criteria"
        after = remote("for p in /sys/class/net/*; do printf '%s\\n' \"${p##*/}\"; done", "interfaces-after")
    report.update({"directory_removed": workspace.removed, "tmp_free_kib_before": workspace.free_kib})
    report["interface_names_unchanged"] = before.returncode == after.returncode == 0 and before.stdout == after.stdout
    if not report["interface_names_unchanged"]:
        failure = "interface enumeration changed or could not be checked"
    report["probe_passed"] = failure is None
    (output / "device-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"Device directory: {directory}")
    print(f"Native ARM32 loader: exit={native.returncode}, supported={report['native_arm32_supported']}")
    if native.stderr:
        print(native.stderr.strip())
    if probe is not None:
        print(f"{report['execution']}: loader={listed.returncode}, probe={probe.returncode}")
        print(probe.stderr.strip())
    if failure:
        raise SystemExit(failure)
    print("PASS: probe only; no database backend, Controller or network configuration started")


if __name__ == "__main__":
    main()
