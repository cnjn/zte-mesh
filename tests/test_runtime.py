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
    return (REPO / path).read_text()

# member name in the archive -> the file it must be a copy of
SOURCES = {
    "ns-run.c": REPO / "ns-run.c",
    "controller-net.c": REPO / "controller-net.c",
    "enroller.c": REPO / "enroller.c",
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
    for marker in (b"config-reject", b"wlan-plan"):
        check(f"db-engine-probe in the archive carries {marker.decode()}", marker in probe,
              "the packed copy predates the current sources; rebuild db-engine-probe with "
              "build-lab-tools.yml (lab-tools artifact) and repack with openwrt/repack-runtime.py --replace")

    # The compiled halves are machine code, so they cannot ride in an arch:all apk: on the
    # x86_64 lab box an aarch64 ns-run would not exec at all. zte-mesh is arch:all (scripts
    # plus the runtime tarball, which is data), and ns-run / mesh-search have their own
    # arch-tagged package. package-pack.mk reads one global PKGARCH per Makefile, so two
    # architectures genuinely require two package directories -- this is the guard against
    # quietly moving a binary back and breaking the lab box again.
    makefile = text("openwrt/zte-mesh/Makefile")
    check("zte-mesh is architecture independent", "PKGARCH:=all" in makefile,
          "the package still claims a board arch; the x86_64 lab box cannot install it")
    check("zte-mesh ships no compiled launcher",
          "/usr/libexec/zte-mesh/ns-run" not in makefile
          and "/usr/libexec/zte-mesh/mesh-search" not in makefile,
          "a binary built by $(TARGET_CC) cannot be installed by an arch:all package")
    check("zte-mesh overrides Build/Compile", "define Build/Compile" in makefile,
          "undefined, it falls back to Build/Compile/Default, which runs $(MAKE) in the build "
          "directory -- there is no makefile there, only the unpacked tarball")

    launcher = text("openwrt/zte-mesh-launcher/Makefile")
    check("the launcher builds ns-run from the archive sources",
          "$(PKG_BUILD_DIR)/ns-run.c" in launcher and "$(PKG_BUILD_DIR)/controller-net.c" in launcher)
    check("the launcher builds mesh-search from the archive sources",
          "$(PKG_BUILD_DIR)/enroller.c" in launcher)
    check("the launcher installs both binaries",
          "/usr/libexec/zte-mesh/ns-run" in launcher
          and "/usr/libexec/zte-mesh/mesh-search" in launcher)
    check("the launcher is arch-tagged", "PKGARCH:=aarch64_cortex-a53" in launcher)
    check("the launcher takes the sources from the sibling runtime tarball",
          "../zte-mesh/zte-mesh-runtime.tar.gz" in launcher,
          "the sources must not be duplicated; they travel inside the tarball (see C5)")

    init = (REPO / "openwrt/zte-mesh/files/zte-mesh.init").read_text()
    check("the init script starts the search helper", "MESH_SEARCH" in init and "procd_open_instance search" in init)

    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
