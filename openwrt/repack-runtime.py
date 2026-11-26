#!/usr/bin/env python3
"""Refresh the runtime tarball with libraries built somewhere else.

`make-runtime.py` is the authority for the archive: it audits the vendor ABI and
writes the layout, the modes and SHA256SUMS from scratch, which needs the
extracted factory image and the ARM toolchain. This script is the smaller
operation that turned out to be needed more often: keep every member byte for
byte, swap in a library that CI built against the factory glibc, and rewrite
SHA256SUMS to match.

Two reasons it exists:

  * libzte_mesh_shim.so in the committed tarball predates the shmat/exec fixes
    the router needed, so a plain `make package/zte-mesh` produced a device that
    failed differently from the one under test.
  * libzte_agent_mm.so was in spawn_daemon()'s LD_PRELOAD from the start and
    never got packed at all, which is why enrolment ended in a -3 from the WLAN
    setter and another M1.

Order, modes and the SHA256SUMS format follow make-runtime.py so the result is
the same archive it would have produced with those files in out/.

  python3 openwrt/repack-runtime.py \\
      --replace root/lib/libzte_mesh_shim.so=prebuilt/libzte_mesh_shim.so \\
      --add     root/lib/libzte_agent_mm.so=prebuilt/libzte_agent_mm.so
"""
import argparse
import hashlib
import io
import sys
import tarfile
from pathlib import Path

EXECUTABLE = {"root/bin/zte-meshctl", "root/bin/db-engine-probe", "root/bin/multiapd", "root/lib/ld-linux.so.3",
              "ns-run", "attach"}
REPO = Path(__file__).resolve().parents[1]
DEFAULT_ARCHIVE = REPO / "openwrt/zte-mesh/zte-mesh-runtime.tar.gz"


def pairs(values):
    out = {}
    for value in values or []:
        if "=" not in value:
            sys.exit("expected NAME=PATH, got " + value)
        name, path = value.split("=", 1)
        out[name] = Path(path)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--output", type=Path, help="write here instead of replacing the archive")
    parser.add_argument("--replace", nargs="*", metavar="NAME=PATH", help="overwrite an existing member")
    parser.add_argument("--add", nargs="*", metavar="NAME=PATH", help="append a new member")
    args = parser.parse_args()

    replace, add = pairs(args.replace), pairs(args.add)
    if not replace and not add:
        sys.exit("nothing to do: pass --replace and/or --add")
    output = args.output or args.archive

    with tarfile.open(args.archive, "r:gz") as source:
        members = [(item, source.extractfile(item).read() if item.isfile() else None) for item in source.getmembers()]
    names = {item.name for item, _ in members}
    for name in list(replace) + list(add):
        if name == "SHA256SUMS":
            sys.exit("SHA256SUMS is regenerated, not replaced")
        if name in replace and name not in names:
            sys.exit(f"{name} is not in the archive; use --add")
        if name in add and name in names:
            sys.exit(f"{name} is already in the archive; use --replace")

    body = []
    for item, data in members:
        if item.name == "SHA256SUMS":
            continue                      # rewritten at the end, from the final bytes
        if item.name in replace:
            data = replace[item.name].read_bytes()
            item.size = len(data)
            item.mode = 0o755 if item.name in EXECUTABLE else 0o644
        body.append((item, data))
    for name, path in sorted(add.items()):
        item = tarfile.TarInfo(name)
        data = path.read_bytes()
        item.size, item.mode = len(data), 0o755 if name in EXECUTABLE else 0o644
        body.append((item, data))

    sums = []
    with tarfile.open(output, "w:gz") as target:
        for item, data in body:
            if data is not None:
                sums.append(f"{hashlib.sha256(data).hexdigest()}  {item.name}\n")
            target.addfile(item, io.BytesIO(data) if data is not None else None)
        text = "".join(sums).encode()
        item = tarfile.TarInfo("SHA256SUMS")
        item.size, item.mode = len(text), 0o644
        target.addfile(item, io.BytesIO(text))
    print(f"{output}: {output.stat().st_size} bytes, {len([1 for i, _ in body if i.isfile()])} files, "
          f"replaced={sorted(replace)} added={sorted(add)}")


if __name__ == "__main__":
    main()
