#!/usr/bin/env python3
"""Run the real vendor DB server/client experiment in private device namespaces.

Requires ns-run's IPC/network/mount isolation; never falls back to host IPC.
No multiapd/cspd, Controller, hardware nodes or real network are deployed here.
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
from schema import registry, policy_registry, POLICY_TABLES, verify_lengths
from vendor_elf import ELF, audit
from profile_schema import SPECS as PROFILE_SPECS, registry as profile_registry, verify_lengths as verify_profile_lengths


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
    schema = registry()
    verify_lengths(output / "runtime/lib/libdb.so")
    (output / "multiap-cfg-schema.json").write_text(json.dumps(schema, indent=2, sort_keys=True) + "\n")
    policies = [policy_registry(name) for name in POLICY_TABLES]
    (output / "mesh-policy-schema.json").write_text(json.dumps(policies, indent=2, sort_keys=True) + "\n")
    profiles = [profile_registry(name) for name in PROFILE_SPECS]
    verify_profile_lengths(output / "runtime/lib/libdb.so")
    (output / "wlan-profile-schema.json").write_text(json.dumps(profiles, indent=2, sort_keys=True) + "\n")
    libraries = {name: output / "runtime" / name for name in manifest["files"] if name.startswith("lib/")}
    probe = output / "db-engine-probe"
    helper = output / "ns-run"
    shim = output / "runtime/lib/libzte_mesh_shim.so"
    elf = ELF.read(probe)
    elf.require_vendor_abi()
    objects = {Path(name).name: ELF.read(path) for name, path in libraries.items()}
    objects[probe.name] = elf
    objects[shim.name] = ELF.read(shim)
    objects[shim.name].require_vendor_abi()
    abi = audit(objects)
    if abi["missing_strong_symbols"] or abi["missing_versions"]:
        parser.error(f"probe ABI failure: {abi}")
    header = helper.read_bytes()[:64]
    if header[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<H", header, 18)[0] != 183:
        parser.error("ns-run must be an AArch64 Linux executable")
    selected = {f"root/{name}": path for name, path in libraries.items()}
    selected.update({"root/bin/db-engine-probe": probe, "root/lib/libzte_mesh_shim.so": shim, "ns-run": helper})
    archive = output / "device-db-probe.tar.gz"
    sums = []
    with tarfile.open(archive, "w:gz") as tar:
        for directory in ("root/tmp", "root/dev/shm"):
            entry = tarfile.TarInfo(directory)
            entry.type, entry.mode = tarfile.DIRTYPE, 0o700
            tar.addfile(entry)
        for name, path in sorted(selected.items()):
            if path.is_symlink():
                parser.error(f"refusing symlink: {name}")
            data = path.read_bytes()
            sums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
            entry = tarfile.TarInfo(name)
            entry.size, entry.mode = len(data), 0o700 if name in ("ns-run", "root/bin/db-engine-probe", "root/lib/ld-linux.so.3") else 0o600
            tar.addfile(entry, io.BytesIO(data))
        data = "".join(sums).encode()
        entry = tarfile.TarInfo("SHA256SUMS")
        entry.size, entry.mode = len(data), 0o600
        tar.addfile(entry, io.BytesIO(data))
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", args.host]
    logs = output / "device-db"
    logs.mkdir(exist_ok=True)

    def remote(command, label):
        result = subprocess.run(ssh + [command], capture_output=True, text=True, timeout=30)
        (logs / f"{label}.stdout").write_text(result.stdout)
        (logs / f"{label}.stderr").write_text(result.stderr)
        return result

    with DeviceWorkspace(remote, archive, "/tmp/zte-mesh-db.") as workspace:
        directory = workspace.directory
        quote = shlex.quote
        snapshot = "for p in /sys/class/net/*; do printf '%s\\n' \"${p##*/}\"; done; printf 'HOST_IPC\\n'; for p in /proc/sysvipc/shm /proc/sysvipc/sem; do test -r \"$p\" || exit 1; cat \"$p\"; done"
        before = remote(snapshot, "host-before")
        if before.returncode:
            raise SystemExit("cannot snapshot host IPC; refusing experiment")
        with archive.open("rb") as stream:
            uploaded = subprocess.run(ssh + [f"tar -xz -C {quote(directory)}"], stdin=stream, capture_output=True, text=True, timeout=120)
        if uploaded.returncode:
            raise SystemExit(f"DB experiment upload failed: {uploaded.stderr[-1000:]}")
        checked = remote(f"cd {quote(directory)} && sha256sum -c SHA256SUMS", "checksums")
        if checked.returncode:
            raise SystemExit("DB experiment checksum failure")
        result = remote(shlex.join([directory + "/ns-run", directory + "/root", "/lib/ld-linux.so.3", "--library-path", "/lib", "/bin/db-engine-probe"]), "probe")
        after = remote(snapshot, "host-after")
    unchanged = before.returncode == after.returncode == 0 and before.stdout == after.stdout
    isolated = "private_ipc=1 private_net=1 readonly_root=1 caps=IPC_OWNER" in result.stderr
    traced = "return=DBShmCliInit returned=1" in result.stderr and "call=dbAPIGetView view=MultiAPCfg" in result.stderr
    cfg_passed = "cfg_get_set=1 cmapi_get=1 negative_queries=1 buffer_guards=1 payload_logged=0" in result.stdout
    policies_passed = ("master_get_set=1 domain_get_set=1 domain_rows=2 cmapi_policy_get=1 "
                       "cmapi_domain_list=1 domain_filter=1 row_isolation=1 payload_logged=0" in result.stdout)
    profiles_passed = ("profile_tables=5 profile_fields=180 identity_get=1 cf_profile_get=1 "
                       "profile_get_set=1 profile_row_isolation=1 profile_guards=1 "
                       "source_defaults_read=0 hardware_calls=0 payload_logged=0" in result.stdout)
    canaries = ("zte-shim-lab-not-a-live-key", "zte-shim-lab-rotated-key", "PUBLIC-NOT-DEVICE-SERIAL")
    redacted = not any(value in result.stdout + result.stderr for value in canaries)
    passed = (result.returncode == 0 and isolated and traced and cfg_passed and policies_passed and
              profiles_passed and redacted and unchanged and
              "PASS shared_counter=45 native_engine=1 multiap_cfg_ready=1 policies_ready=1 "
              "profile_fixture_ready=1 controller_ready=0" in result.stdout)
    report = {"host": args.host, "directory": directory, "directory_removed": workspace.removed,
              "tmp_free_kib_before": workspace.free_kib, "execution": "native-arm32", "probe_exit": result.returncode,
              "private_namespaces": isolated, "configuration_view_traced": traced,
              "host_ipc_and_interface_names_unchanged": unchanged,
              "vendor_abi": abi, "probe_elf": elf.summary(), "database_engine_passed": passed,
              "controller_ready": False, "schema_ready": False,
              "multiap_cfg_schema_ready": passed, "configuration_read_write_passed": passed,
              "configuration_fields": 50, "configuration_view_bytes": 2952,
              "cmapi_configuration_marshaled": passed,
              "multiap_master_schema_ready": passed, "multiap_domain_schema_ready": passed,
              "policy_read_write_passed": passed, "domain_row_isolation_passed": passed,
              "cmapi_domain_list_passed": passed, "domain_filter_passed": passed,
              "cross_process_write_readback_passed": passed,
              "policy_fields": {"MultiAPMaster": 55, "MultiAPDomain": 106},
              "wlan_profile_schema_metadata_ready": passed, "identity_schema_metadata_ready": passed,
              "profile_fixture_read_write_passed": passed, "profile_row_isolation_passed": passed,
              "cf_profile_get_passed": passed, "profile_credential_canaries_redacted": redacted,
              "profile_table_fields": {p["view"]: len(p["fields"]) for p in profiles},
              "profile_source_defaults_read": False, "wlan_profile_ready": False,
              "fixture": "public-test-only; synthetic profiles/identity; no real credentials or Controller activation"}
    (output / "device-db-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"DB experiment directory: {directory}")
    print(result.stdout.strip())
    print(result.stderr.strip())
    print(f"probe_exit={result.returncode} isolated={isolated} host_state_unchanged={unchanged}")
    if not passed:
        raise SystemExit("DB experiment failed; no host-IPC fallback permitted")


if __name__ == "__main__":
    main()
