#!/usr/bin/env python3
"""Bounded actual multiapd startup in private fixture DB/net/mount namespaces.
No host LAN, hardware devices, driver stubs or real credentials. Not a service.
"""
import argparse
import hashlib
import io
import json
import re
import shlex
import subprocess
import sys
import tarfile
from pathlib import Path

from device_workspace import DeviceWorkspace
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
    subprocess.run([sys.executable, str(Path(__file__).with_name("startup-schema.py")),
                    "--cspd", str(output / "analysis/cspd"), "--libdb", str(output / "runtime/lib/libdb.so"),
                    "--verify", str(Path(__file__).with_name("startup-views.inc"))], check=True)
    files = {"root/" + n: output / "runtime" / n for n in manifest["files"]}
    files.update({"root/bin/db-engine-probe": output / "db-engine-probe",
                  "root/lib/libzte_mesh_shim.so": output / "runtime/lib/libzte_mesh_shim.so",
                  "root/lib/libzte_controller_trace.so": output / "runtime/lib/libzte_controller_trace.so",
                  "root/lib/libzte_controller_route.so": output / "runtime/lib/libzte_controller_route.so",
                  "root/lib/libzte_controller_profile.so": output / "runtime/lib/libzte_controller_profile.so",
                  "root/bin/zte-meshctl": output / "zte-meshctl",
                  "root/lib/liboss_pub.so": output / "runtime-extra/lib/liboss_pub.so",
                  "ns-run": output / "ns-run"})
    objects = {Path(n).name: ELF.read(p) for n, p in files.items() if n.startswith("root/")}
    for obj in objects.values():
        obj.require_vendor_abi()
    abi = audit(objects)
    if abi["missing_strong_symbols"] or abi["missing_versions"]:
        parser.error("factory bootstrap ABI mismatch")
    subprocess.run([sys.executable, str(Path(__file__).with_name("cm-profile-evidence.py")),
                    "--libcmapi", str(output / "runtime/lib/libcmapi.so"),
                    "--multiapd", str(output / "runtime/bin/multiapd"),
                    "--verify", str(Path(__file__).with_name("cm-profile-layout.inc"))], check=True)
    archive = output / "device-controller-probe.tar.gz"
    sums = []
    with tarfile.open(archive, "w:gz") as tar:
        item = tarfile.TarInfo("root/dev/urandom")
        item.size, item.mode = 0, 0o600
        tar.addfile(item, io.BytesIO(b""))
        sums.append(f"{hashlib.sha256(b'').hexdigest()}  root/dev/urandom\n")
        for name in ("root/tmp", "root/dev/shm", "root/var"):
            item = tarfile.TarInfo(name)
            item.type, item.mode = tarfile.DIRTYPE, 0o700
            tar.addfile(item)
        # Factory OSS uses pathname Unix sockets under /var/tmp; point only
        # private runtime paths to private tmpfs, never to host /var.
        for name in ("root/var/tmp", "root/var/run", "root/var/log"):
            item = tarfile.TarInfo(name)
            item.type, item.linkname = tarfile.SYMTYPE, "/tmp"
            tar.addfile(item)
        for name, path in sorted(files.items()):
            if path.is_symlink():
                parser.error("refusing package symlink")
            data = path.read_bytes()
            sums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
            item = tarfile.TarInfo(name)
            item.size = len(data)
            item.mode = 0o700 if name in ("ns-run", "root/bin/db-engine-probe", "root/bin/zte-meshctl", "root/bin/multiapd", "root/lib/ld-linux.so.3") else 0o600
            tar.addfile(item, io.BytesIO(data))
        data = "".join(sums).encode()
        item = tarfile.TarInfo("SHA256SUMS")
        item.size, item.mode = len(data), 0o600
        tar.addfile(item, io.BytesIO(data))
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", args.host]
    logs = output / "device-controller"
    logs.mkdir(exist_ok=True)

    def remote(command, name, timeout=30):
        r = subprocess.run(ssh + [command], capture_output=True, text=True, timeout=timeout)
        (logs / (name + ".stdout")).write_text(r.stdout)
        (logs / (name + ".stderr")).write_text(r.stderr)
        return r

    with DeviceWorkspace(remote, archive, "/tmp/zte-mesh-db.ctrl.") as workspace:
        directory = workspace.directory
        snapshot = "for p in /sys/class/net/*; do printf '%s\\n' \"${p##*/}\"; done; printf 'HOST_IPC\\n'; for p in /proc/sysvipc/shm /proc/sysvipc/sem /proc/sysvipc/msg; do test -r \"$p\" || exit 1; cat \"$p\"; done"
        before = remote(snapshot, "host-before")
        if before.returncode:
            raise SystemExit("host IPC snapshot unavailable; refusing startup")
        with archive.open("rb") as stream:
            r = subprocess.run(ssh + [f"tar -xz -C {shlex.quote(directory)}"], stdin=stream, capture_output=True, text=True, timeout=120)
        if r.returncode or remote(f"cd {shlex.quote(directory)} && sha256sum -c SHA256SUMS", "checksums").returncode:
            raise SystemExit("startup upload/checksum failed")
        command = [directory + "/ns-run", directory + "/root", "/lib/ld-linux.so.3", "--library-path", "/lib", "/bin/db-engine-probe", "--controller-startup"]
        result = remote(shlex.join(command), "probe", 65)
        after = remote(snapshot, "host-after")
    unchanged = before.returncode == after.returncode == 0 and before.stdout == after.stdout
    observed = result.returncode == 0 and "ZTE_CONTROLLER_STARTUP observed=1" in result.stdout
    isolated = "private_ipc=1 private_net=1 readonly_root=1 caps=IPC_OWNER,NET_RAW private_lab_bridges=2 private_veth=1" in result.stderr
    milestones = {"master_init": "vendor_log_format=MASTER INIT" in result.stderr,
                  "local_1905_device": "vendor_log_format=Creating a new 1905Device:" in result.stderr,
                  "packet_socket_1905": bool(re.search(r"socket family=17 type=3 protocol=14985 success=1", result.stderr)),
                  "route_adapter_verified": "real_route_adapter=1 buffer_guard=1 unknown_interface_rejected=1" in result.stdout,
                  "subscriber_server_verified": "real_subscriber_server=1 shared_bytes=131072 lock_verified=1" in result.stdout}
    milestones["kernel_entropy_verified"] = "kernel_entropy=1 native_crypto_rng=1" in result.stdout
    milestones["cm_profiles_verified"] = "real_cm_profiles=1 readonly=1 buffer_guard=1 rejected_unchanged=1" in result.stdout
    rx_types = sorted(set(int(m) for m in re.findall(r" rx1905 cmdu=(\d+) ", result.stderr)))
    tx_types = sorted(set(int(m) for m in re.findall(r" tx1905 cmdu=(\d+) ", result.stderr)))
    milestones["discovery_and_query_received"] = 0 in rx_types and 2 in rx_types
    autoconfig_bands = sorted(set(int(m) for m in re.findall(
        r"fixture=autoconfig-response private_only=1 peer_received=1 cmdu=8 band=(\d+) ", result.stdout)))
    milestones["autoconfig_both_bands_peer_verified"] = autoconfig_bands == [0, 1] and 8 in tx_types
    milestones["wsc_m1_received"] = 9 in rx_types and "wsc_status vendor_format=M1 len:" in result.stderr
    milestones["private_ingress_binding"] = "ZTE_CONTROLLER_WIRE actual_lab_bind=1 status=0 host_link=0" in result.stderr
    milestones["topology_response_peer_verified"] = (
        "fixture=topology-response private_only=1 peer_received=1 cmdu=3 mid_match=1 tlv_sane=1 device_info=1" in result.stdout
        and 3 in tx_types)
    redacted = not any(s in result.stdout + result.stderr for s in (
        "public-test-only-key", "zte-controller-test", "zte-shim-lab-not-a-live-key"))
    report = {"host": args.host, "directory": directory, "probe_exit": result.returncode,
              "real_daemon_observed": observed, "isolated": isolated, "directory_removed": workspace.removed,
              "tmp_free_kib_before": workspace.free_kib,
              "host_ipc_and_interface_names_unchanged": unchanged, "vendor_abi": abi,
              "trace": [l for l in result.stderr.splitlines() if l.startswith("ZTE_CONTROLLER_TRACE ")],
              "controller_ready": False, "real_credentials_read": False, "host_network_access": False,
              "milestones": milestones, "public_credentials_redacted": redacted,
              "tx1905_observed": " tx1905 " in result.stderr,
              "rx1905_observed": " rx1905 " in result.stderr,
              "rx1905_types": rx_types, "tx1905_types": tx_types,
              "topology_response_peer_verified": milestones["topology_response_peer_verified"],
              "autoconfig_response_bands": autoconfig_bands,
              "wsc_m2_structure_observed": "fixture=wsc-m2 private_only=1 peer_received=1" in result.stdout,
              "wsc_cryptographic_verified": "cryptographic_verified=1 keywrap_verified=1 profile_match=1" in result.stdout,
              "packet_trace_budget_exhausted": "log_budget=packet exhausted=1" in result.stderr,
              "ordinary_trace_budget_exhausted": "log_budget=ordinary exhausted=1" in result.stderr,
              "native_dh_keygen_succeeded": "native_dh_keygen status=1" in result.stderr,
              "public_packet_fixture": [l for l in result.stdout.splitlines() if l.startswith("ZTE_CONTROLLER_PACKET ")]}
    (output / "device-controller-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print("Actual-daemon startup directory:", directory)
    print(result.stdout.strip())
    print("\n".join(l for l in result.stderr.splitlines() if l.startswith("ZTE_CONTROLLER_TRACE ") or l.startswith("ZTE_CM_PROFILE ") or l.startswith("ZTE_CONTROLLER_WIRE ") or " fail=" in l or " error=" in l))
    print(f"probe_exit={result.returncode} real_daemon_observed={observed} isolated={isolated} host_state_unchanged={unchanged}")
    print("Startup milestones:", milestones, "public_credentials_redacted=", redacted)
    if not observed or not isolated or not unchanged or not redacted or not all(milestones.values()) or not workspace.removed:
        raise SystemExit("actual-daemon observation failed; no host fallback")


if __name__ == "__main__":
    main()
