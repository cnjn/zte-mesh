#!/usr/bin/env python3
"""Rebuild the compile-time `out/` tree from the runtime bundle committed in the repo.

`prepare.py` is the authority for `out/`: it derives the multiapd dependency
closure from the pinned factory JFFS2 and records a hash for every file.  A
machine without that image cannot run it, so this script reconstructs the same
shape from `openwrt/zte-mesh/zte-mesh-runtime.tar.gz` (which is produced from
the audited `out/` and ships its own `SHA256SUMS`).

What it produces:

    out/runtime/bin/multiapd          multiapd, hash-checked against prepare.py
    out/runtime/lib/*.so*             the same dependency closure prepare.py walks
    out/runtime-extra/lib/liboss_pub.so
    out/manifest.json                 synthetic: real hashes, but no rootfs hash

The manifest carries `"source": "runtime-bundle"` and empty rootfs/kmodule
hashes, so it is never mistaken for the audited bundle: `prepare.py` rejects it
(its `verify_existing` compares the rootfs hash) and this script refuses to
overwrite a manifest that does not carry that marker.

    python3 tools/build/bootstrap-out.py            # -> ./out
    python3 tools/build/bootstrap-out.py --force    # replace a previous synthetic out/
    python3 tools/build/bootstrap-out.py --out DIR
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tarfile
import tempfile
from collections import deque
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from prepare import LIBOSS_PUB_SHA256, MULTIAPD_SHA256  # noqa: E402
from vendor_elf import ELF, audit  # noqa: E402

BUNDLE = REPO / "openwrt/zte-mesh/zte-mesh-runtime.tar.gz"
MARKER = "runtime-bundle"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_bundle(bundle: Path) -> tuple[dict[str, bytes], dict[str, str]]:
    """Return the bundle's files plus its own SHA256SUMS, after verifying them."""
    files: dict[str, bytes] = {}
    with tarfile.open(bundle, "r:gz") as archive:
        for member in archive.getmembers():
            if not member.isfile():
                continue
            handle = archive.extractfile(member)
            if handle is None:
                continue
            files[member.name] = handle.read()

    sums = files.get("SHA256SUMS")
    if sums is None:
        raise SystemExit(f"{bundle} has no SHA256SUMS; refusing to trust it")
    expected: dict[str, str] = {}
    for line in sums.decode().splitlines():
        if not line.strip():
            continue
        value, name = line.split(None, 1)
        expected[name.strip()] = value

    for name, value in expected.items():
        if name not in files:
            raise SystemExit(f"bundle lists {name} but does not contain it")
        actual = digest(files[name])
        if actual != value:
            raise SystemExit(f"{name}: bundle hash {actual} != SHA256SUMS {value}")

    unlisted = sorted(set(files) - set(expected) - {"SHA256SUMS"})
    if unlisted:
        print(f"  note: {len(unlisted)} bundle entries are not in SHA256SUMS: {', '.join(unlisted)}")
    return files, expected


def closure(files: dict[str, bytes]) -> list[str]:
    """Walk multiapd's DT_NEEDED the way prepare.prepare() does."""
    library = {name[len("root/lib/"):]: data for name, data in files.items() if name.startswith("root/lib/")}
    main = ELF(files["root/bin/multiapd"])
    main.require_vendor_abi()
    if main.interpreter != "/lib/ld-linux.so.3":
        raise SystemExit(f"unexpected factory interpreter {main.interpreter!r}")

    objects = {"multiapd": main}
    pending = deque(["ld-linux.so.3", *main.needed])
    seen: list[str] = []
    while pending:
        name = pending.popleft()
        if "/" in name or name in (".", ".."):
            raise SystemExit(f"unsafe DT_NEEDED entry {name!r}")
        if name in seen:
            continue
        if name not in library:
            raise SystemExit(f"bundle lacks {name}, needed by the multiapd closure")
        obj = ELF(library[name])
        obj.require_vendor_abi()
        seen.append(name)
        objects[name] = obj
        pending.extend(obj.needed)

    result = audit(objects)
    if result["missing_strong_symbols"] or result["missing_versions"]:
        raise SystemExit(f"dependency audit failed: {json.dumps(result, indent=2)}")
    print(f"  closure: {len(seen)} libraries, audit clean "
          f"({len(result.get('missing_versions', []))} unresolved versions)")
    return seen


def write(path: Path, data: bytes, mode: int) -> dict:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(mode)
    return {"size": len(data), "sha256": digest(data)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bundle", type=Path, default=BUNDLE)
    parser.add_argument("--out", type=Path, default=REPO / "out")
    parser.add_argument("--force", action="store_true", help="replace an existing synthetic out/")
    args = parser.parse_args()

    out = args.out.absolute()
    manifest_path = out / "manifest.json"
    if manifest_path.exists():
        try:
            existing = json.loads(manifest_path.read_text())
        except json.JSONDecodeError:
            existing = {}
        if existing.get("source") != MARKER and not args.force:
            raise SystemExit(
                f"{manifest_path} is not a synthetic manifest; refusing to overwrite an audited out/.\n"
                "Use --force only if you are sure this directory can be discarded."
            )
        print(f"replacing existing synthetic {out}")

    print(f"reading {args.bundle.relative_to(REPO)}")
    files, sums = read_bundle(args.bundle)

    program = files["root/bin/multiapd"]
    if digest(program) != MULTIAPD_SHA256:
        raise SystemExit("bundle multiapd does not match the pinned factory hash")

    libs = closure(files)
    print(f"writing {out}")
    for directory in ("runtime", "runtime-extra"):
        if (out / directory).is_symlink():
            raise SystemExit(f"{out / directory} must not be a symlink")
        if (out / directory).exists():
            shutil.rmtree(out / directory)

    entries = {"bin/multiapd": write(out / "runtime/bin/multiapd", program, 0o700)}
    for name in libs:
        # Same mode rules as prepare.prepare(): the loader has to be executable,
        # every other library stays read-only.
        entries[f"lib/{name}"] = write(out / "runtime/lib" / name, files[f"root/lib/{name}"],
                                       0o700 if "ld-linux" in name else 0o600)

    oss_pub = files["root/lib/liboss_pub.so"]
    if digest(oss_pub) != LIBOSS_PUB_SHA256:
        raise SystemExit("bundle liboss_pub.so does not match the pinned client-library hash")
    write(out / "runtime-extra/lib/liboss_pub.so", oss_pub, 0o600)

    manifest = {
        "format": 1,
        "source": MARKER,
        "source_bundle": str(args.bundle.relative_to(REPO)),
        "rootfs_sha256": "",
        "kmodule_sha256": "",
        "multiapd_sha256": MULTIAPD_SHA256,
        "library_count": len(libs),
        "files": entries,
        "limitations": [
            "Derived from the committed runtime bundle, not from the factory JFFS2: "
            "prepare.py cannot verify this tree and will rebuild it from the image.",
            "The bundle is the audited out/ from a machine that had the image; SHA256SUMS inside it is the provenance link.",
        ],
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    print(f"  multiapd + {len(libs)} libraries -> {out}/runtime")
    print(f"  manifest -> {manifest_path} (synthetic, source={MARKER})")
    print("next: make -j$(nproc) all database")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
