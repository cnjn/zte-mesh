#!/usr/bin/env python3
"""The runtime tarball must carry the sources the package builds.

zte-mesh-runtime.tar.gz used to be the only place ns-run came from, and the copy
inside it was only as new as the last `sh tools/build/runtime.sh install`. On
2026-10-31 it was an AArch64 build from ns-run.c before the emulator branch: no
ZTE_MESH_QEMU, so on an arm64 kernel without CONFIG_COMPAT the guest exec died
with "execv errno=8" and the controller instance respawned every five seconds
(FEATURE_STATUS C5). Nothing on the host could have caught that, because nothing
on the host looked at the tarball.

The package now compiles ns-run and mesh-search from sources that travel inside
the same archive, so these checks are the drift guard:

  * the three sources are in the archive and identical to the repository files
  * SHA256SUMS covers them and matches the bytes
  * the archive is still the router one (its ns-run is AArch64)
  * the Makefile builds them from those members, and the init script runs the
    helper it installs

Run directly:  python3 tests/test_runtime.py
"""
import hashlib
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ARCHIVE = REPO / "openwrt/zte-mesh/zte-mesh-runtime.tar.gz"


def text(path):
    return (REPO / path).read_text(encoding="utf-8")

# member name in the archive -> the file it must be a copy of
SOURCES = {
    "ns-run.c": REPO / "src" / "ns-run.c",
    "controller-net.c": REPO / "src" / "controller-net.c",
    "enroller.c": REPO / "tools/lab/enroller.c",
}

failures = []


def check(label, condition, detail=""):
    print(("ok   " if condition else "FAIL ") + label + (f"  {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(label)


def main():
    if not ARCHIVE.is_file():
        print(f"FAIL runtime tarball is missing: {ARCHIVE}")
        return 1

    with tarfile.open(ARCHIVE, "r:gz") as tar:
        members = {m.name: m for m in tar.getmembers()}
        blobs = {name: tar.extractfile(m).read() for name, m in members.items() if m.isfile()}

    for name, original in SOURCES.items():
        check(f"{name} is in the runtime tarball", name in blobs)
        if name not in blobs:
            continue
        check(f"{name} is a byte-for-byte copy of {original.name}", blobs[name] == original.read_bytes(),
              "regenerate with: sh tools/build/runtime.sh install")
        check(f"{name} is not executable in the archive", (members[name].mode & 0o111) == 0,
              f"mode={oct(members[name].mode)}")

    sums = blobs.get("SHA256SUMS", b"").decode()
    parsed = {}
    for line in sums.splitlines():
        digest, _, name = line.partition("  ")
        parsed[name] = digest
    for name in SOURCES:
        check(f"SHA256SUMS covers {name}", name in parsed)
        if name in parsed:
            check(f"SHA256SUMS matches {name}", parsed[name] == hashlib.sha256(blobs[name]).hexdigest())

    # The archive is the router one: ns-run is built for AArch64 (the lab copy in out/ is not).
    ns_run = blobs.get("ns-run", b"")
    machine = int.from_bytes(ns_run[18:20], "little") if ns_run[:4] == b"\x7fELF" else 0
    check("ns-run in the archive is an AArch64 ELF", ns_run[:4] == b"\x7fELF" and machine == 183,
          f"e_machine={machine}")

    # Same hazard, other binary: root/bin/db-engine-probe is the ARM32 host side of the sandbox
    # (parse_config, the service loop). It is linked -nostdlib against the factory glibc, so it can
    # only be rebuilt in the lab container (build-lab-tools.yml) -- it cannot be compiled here the
    # way ns-run now is.
    # A copy frozen at some earlier pack date silently keeps the device on old behaviour while the
    # init script already feeds it new keys: on 2026-10-31 the packed copy predated the child role,
    # so its parse_config refused "mesh.role=slave", controller_service() returned -1 and the
    # controller instance respawned every five seconds on both lab boxes (2/3 instances) with
    # "ZTE_MESH_SERVICE fail=config-missing-or-invalid" and no detail line. These two markers are
    # the cheapest proof the copy is not that one: both are absent from it and present in a build
    # made from these sources (config-reject from controller-service.c, wlan-plan from agent-sync.c).
    probe = blobs.get("root/bin/db-engine-probe", b"")
    for marker in (b"config-reject", b"wlan-plan",
                   b"bss_detail",        # the per-BSS rows state.json prints since the 逐路 batch
                   b"ap_metric_query"):  # the controller asks for those counts itself since the 终端数 batch
        check(f"db-engine-probe in the archive carries {marker.decode()}", marker in probe,
              "the packed copy predates the current sources; rebuild prebuilt/db-engine-probe with "
              "build-lab-tools.yml and repack with openwrt/repack-runtime.py --replace")

    # Same hazard on the preload side, and this one is easy to hit: three batches of controller-peer.c /
    # controller-1905.c changes went out with the archive untouched, because nothing on the host looked at
    # the packed .so. A stale copy is not merely old numbers - the write path and the read path are in
    # different binaries, so a service that sums per-BSS files with a trace library that never writes them
    # falls back to the digest every second and the page silently keeps the behaviour of the previous batch.
    # The marker is the file name format only the per-BSS write builds.
    trace = blobs.get("root/lib/libzte_controller_trace.so", b"")
    marker = b".%02x:%02x:%02x:%02x:%02x:%02x.sta"
    check("libzte_controller_trace.so in the archive carries the per-BSS file name", marker in trace,
          "the packed library predates controller-peer.c's write_bss(); rebuild the ARM32 shim and repack "
          "with openwrt/repack-runtime.py --replace root/lib/libzte_controller_trace.so=...")
    # The same library is where the AP Metric Query is answered now, so one packed .so carries both halves of
    # the terminal count: the writer of the per-BSS files and the reader of /run/zte-mesh/ap-sta.
    check("libzte_controller_trace.so in the archive answers an AP Metric Query", b"ap_metrics_answer" in trace,
          "the packed library predates agent-ap-metrics.c; the child stays silent and the master reads "
          "stations=-1 for a node that has the counts")

    # The launcher is machine code, and an ELF names one architecture -- but it no longer rides in a second
    # package for that reason. zte-mesh is arch:all and ships one build per CPU under arch/<cpu>/ next to a
    # shell entry that picks the directory matching `uname -m`, so the architecture question is answered at
    # run time by that script instead of at install time by the solver. package-pack.mk reads one global
    # PKGARCH per Makefile, which is exactly why the apk cannot name a CPU *and* carry three builds; the
    # guard below is that the payload it needs is really installed by the same Makefile as the scripts.
    makefile = text("openwrt/zte-mesh/Makefile")
    check("zte-mesh is architecture independent", "PKGARCH:=all" in makefile,
          "the package names a board arch; the x86_64 lab box cannot install it")
    check("zte-mesh builds the launcher out of the unpacked tarball",
          "launcher-arches.sh" in makefile
          and "sh $(ZTE_MESH_LAUNCHER_BUILD) $(PKG_BUILD_DIR)" in makefile,
          "the code that gets compiled has to be the code that ships: one unpack of the runtime tarball feeds both")
    check("zte-mesh overrides Build/Compile", "define Build/Compile" in makefile,
          "undefined, it falls back to Build/Compile/Default, which runs $(MAKE) in the build "
          "directory -- there is no makefile there, only the unpacked tarball")
    check("zte-mesh installs the entry script under both names",
          "./files/launcher-dispatch $(1)/usr/libexec/zte-mesh/ns-run" in makefile
          and "/usr/libexec/zte-mesh/mesh-search" in makefile)
    check("zte-mesh installs the per-CPU builds",
          "$(PKG_BUILD_DIR)/arch $(1)/usr/libexec/zte-mesh/" in makefile,
          "an entry script without arch/ answers 'no build for this machine' on every CPU")
    check("zte-mesh still ships the runtime tarball", "zte-mesh-runtime.tar.gz" in makefile)
    check("no package directory is named zte-mesh-launcher",
          not (REPO / "openwrt/zte-mesh-launcher").exists(),
          "the split was merged away; a leftover Makefile would build an apk whose files belong to zte-mesh too")

    init = (REPO / "openwrt/zte-mesh/files/zte-mesh.init").read_text(encoding="utf-8")
    check("the init script starts the search helper", "MESH_SEARCH" in init and "procd_open_instance search" in init)

    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
