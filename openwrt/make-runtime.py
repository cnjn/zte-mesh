#!/usr/bin/env python3
"""Pack the audited vendor runtime + sandbox launcher for the zte-mesh package.

Reuses the same inputs and ABI audit as the device smoke tests; nothing is
fetched. The vendor binaries stay private: the tarball is not a repository file.
"""
import argparse
import hashlib
import io
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare import verify_existing  # noqa: E402
from vendor_elf import ELF, audit  # noqa: E402

LIBOSS_PUB_SHA256 = "7c7403c627cf253b37e335451263a1e2849b4097d20b780de4e4e12a96218e11"  # client OSS library of the same factory image
EXECUTABLE = {"root/bin/zte-meshctl", "root/bin/db-engine-probe", "root/bin/multiapd", "root/lib/ld-linux.so.3", "ns-run", "attach"}


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=repo / "out")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--ns-run", type=Path, help="launcher binary (default: <output>/ns-run)")
    parser.add_argument("--extra", type=Path, nargs="*", default=[], help="extra files stored at the archive top level")
    args = parser.parse_args()
    output = args.output.resolve()
    archive = args.archive or output / "zte-mesh-runtime.tar.gz"
    manifest = verify_existing(output)
    files = {"root/" + n: output / "runtime" / n for n in manifest["files"]}
    for lib in ("libzte_mesh_shim", "libzte_controller_trace", "libzte_controller_route", "libzte_controller_profile",
                "libzte_agent_mm"):
        files[f"root/lib/{lib}.so"] = output / f"runtime/lib/{lib}.so"
    files["root/bin/db-engine-probe"] = output / "db-engine-probe"
    files["root/bin/zte-meshctl"] = output / "zte-meshctl"
    client = output / "runtime-extra/lib/liboss_pub.so"
    if hashlib.sha256(client.read_bytes()).hexdigest() != LIBOSS_PUB_SHA256:
        sys.exit("liboss_pub.so does not match the pinned factory library")
    files["root/lib/liboss_pub.so"] = client
    files["ns-run"] = (args.ns_run or output / "ns-run").resolve()
    for extra in args.extra:
        files[extra.name] = extra.resolve()
    objects = {Path(n).name: ELF.read(p) for n, p in files.items() if n.startswith("root/")}
    for obj in objects.values():
        obj.require_vendor_abi()
    abi = audit(objects)
    if abi["missing_strong_symbols"] or abi["missing_versions"]:
        sys.exit("vendor ABI audit failed")
    sums = []
    with tarfile.open(archive, "w:gz") as tar:
        def directory(name, mode=0o755):
            item = tarfile.TarInfo(name)
            item.type, item.mode = tarfile.DIRTYPE, mode
            tar.addfile(item)

        for name in ("root", "root/tmp", "root/dev", "root/dev/shm", "root/var", "root/run", "root/run/zte-mesh"):
            directory(name)
        item = tarfile.TarInfo("root/dev/urandom")  # bind-mount target for the kernel entropy device
        item.size, item.mode = 0, 0o600
        tar.addfile(item, io.BytesIO(b""))
        for name in ("root/var/tmp", "root/var/run", "root/var/log"):
            item = tarfile.TarInfo(name)
            item.type, item.linkname, item.mode = tarfile.SYMTYPE, "/tmp", 0o777
            tar.addfile(item)
        for name, path in sorted(files.items()):
            if path.is_symlink():
                sys.exit("refusing package symlink: " + name)
            data = path.read_bytes()
            sums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
            item = tarfile.TarInfo(name)
            item.size = len(data)
            item.mode = 0o755 if name in EXECUTABLE else 0o644
            tar.addfile(item, io.BytesIO(data))
        data = "".join(sums).encode()
        item = tarfile.TarInfo("SHA256SUMS")
        item.size, item.mode = len(data), 0o644
        tar.addfile(item, io.BytesIO(data))
    print(f"{archive}: {archive.stat().st_size} bytes, {len(files)} files, ABI audit clean")


if __name__ == "__main__":
    main()
