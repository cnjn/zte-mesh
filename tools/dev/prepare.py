#!/usr/bin/env python3
"""Build a minimal runtime from the pinned factory JFFS2, never from loose ELFs."""
from __future__ import annotations

import argparse
import os
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
from collections import deque
from pathlib import Path

from jffs2 import EvidenceError, extract_file
from vendor_elf import ELF, ELFError, audit
from schema import CSPD_SHA256

ROOTFS_SHA256 = "e7295bb6aa714a62c9856413c18cfedf492fca4d08c49cfc2d8e4e5c19991127"
KMODULE_SHA256 = "3fbb2e29fe7a94124aba084217c372d2694c880c8657781c9c641f7d0d1fad46"
MULTIAPD_SHA256 = "a0ba6beeb1eb4e0c900b24d06e9d37e7174f9f640320d12803d7d80c4276280a"
# Client-side OSS library (the factory command line tools use it instead of liboss.so).
LIBOSS_PUB_SHA256 = "7c7403c627cf253b37e335451263a1e2849b4097d20b780de4e4e12a96218e11"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def checked(data: bytes, expected: str, description: str):
    actual = digest(data)
    if actual != expected:
        raise EvidenceError(f"{description} SHA256 mismatch: {actual}")


def resolve_library(base: Path, name: str) -> Path:
    if not name or "/" in name or name in (".", ".."):
        raise EvidenceError("unsafe library name")
    try:
        path = (base / name).resolve(strict=True)
        path.relative_to(base.resolve())
    except (ValueError, OSError, RuntimeError) as error:
        raise EvidenceError(f"missing/escaping library: {name}") from error
    if not path.is_file():
        raise EvidenceError(f"library is not a regular file: {name}")
    return path


def verify_existing(output: Path) -> dict:
    manifest = json.loads((output / "manifest.json").read_text())
    if manifest.get("rootfs_sha256") != ROOTFS_SHA256 or manifest.get("multiapd_sha256") != MULTIAPD_SHA256:
        raise EvidenceError("existing bundle has different evidence baseline")
    runtime = output / "runtime"
    if runtime.is_symlink():
        raise EvidenceError("existing runtime must not be a symlink")
    for name, metadata in manifest["files"].items():
        if not re.fullmatch(r"bin/multiapd|lib/[A-Za-z0-9_.+-]+", name):
            raise EvidenceError(f"unsafe runtime manifest path: {name}")
        path = runtime / name
        if path.is_symlink() or not path.is_file():
            raise EvidenceError(f"existing runtime file is missing/symlink: {name}")
        checked(path.read_bytes(), metadata["sha256"], name)
    checked((runtime / "bin/multiapd").read_bytes(), MULTIAPD_SHA256, "existing factory multiapd")
    return manifest


def prepare(rootfs: Path, output: Path, unsquashfs: str) -> dict:
    image = rootfs.read_bytes()
    checked(image, ROOTFS_SHA256, "factory rootfs")
    if output.is_symlink() or (output / "runtime").is_symlink():
        raise EvidenceError("output must not be a symlink")
    output.mkdir(parents=True, exist_ok=True)
    if (output / "runtime").exists():
        return verify_existing(output)
    kmodule = extract_file(image, "kmoduletmp/kmodule.img")
    checked(kmodule, KMODULE_SHA256, "nested kmodule image")
    with tempfile.TemporaryDirectory(prefix=".mesh-prepare-", dir=output) as work:
        work = Path(work)
        nested = work / "kmodule.img"
        nested.write_bytes(kmodule)
        extracted = work / "extracted"
        command = [unsquashfs, "-no-progress", "-processors", "1", "-d", str(extracted), str(nested), "bin/multiapd", "lib/*.so*"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise EvidenceError(f"unsquashfs failed: {result.stderr[-1500:]}")
        program = (extracted / "bin/multiapd").read_bytes()
        checked(program, MULTIAPD_SHA256, "factory multiapd")
        main = ELF(program)
        main.require_vendor_abi()
        if main.interpreter != "/lib/ld-linux.so.3":
            raise ELFError("unexpected factory interpreter")
        objects = {"multiapd": main}
        sources = {}
        pending = deque(["ld-linux.so.3", *main.needed])
        while pending:
            name = pending.popleft()
            if name in sources:
                continue
            path = resolve_library(extracted / "lib", name)
            obj = ELF.read(path)
            obj.require_vendor_abi()
            sources[name] = path
            objects[name] = obj
            pending.extend(obj.needed)
        validation = audit(objects)
        if validation["missing_strong_symbols"] or validation["missing_versions"]:
            raise ELFError(f"factory dependency audit failed: {validation}")
        runtime = work / "runtime"
        (runtime / "bin").mkdir(parents=True)
        (runtime / "lib").mkdir()
        files = {}
        for relative, data in [("bin/multiapd", program)] + [(f"lib/{name}", path.read_bytes()) for name, path in sorted(sources.items())]:
            destination = runtime / relative
            destination.write_bytes(data)
            destination.chmod(0o700 if relative.startswith("bin/") or "ld-linux" in relative else 0o600)
            files[relative] = {"size": len(data), "sha256": digest(data)}
        manifest = {
            "format": 1,
            "rootfs_sha256": ROOTFS_SHA256,
            "nested_image_path": "kmoduletmp/kmodule.img",
            "kmodule_sha256": KMODULE_SHA256,
            "multiapd_sha256": MULTIAPD_SHA256,
            "library_count": len(sources),
            "files": files,
            "elf": {name: obj.summary() for name, obj in sorted(objects.items())},
            "audit": validation,
            "limitations": ["Static audit excludes dlopen modules and runtime services.", "This is not a working Controller configuration."],
        }
        categories = {
            "database": r"^db|^DB|Shm",
            "runtime_ipc": r"^OSS|^Oss|^SSEND$|^ASEND$|^SubScrib|^SET_TIMER$|^EXTRA_",
            "platform": r"^Cm|^Cf|^Wlan|^WLAN|^MapGet|^GetIF|^ListAllIF|^PcStart",
        }
        abi = {"binary_sha256": MULTIAPD_SHA256, "signatures_recovered": ["int DBShmCliInit(void)", "int dbAPIGetView(const char *, const char *, void *)"], "imports": {name: sorted(s for s in main.imports if re.search(pattern, s)) for name, pattern in categories.items()}, "entrypoints": {name: metadata for name, metadata in sorted(main.functions.items()) if re.search(r"^(main$|master_init$|baseCreateSockfd$|baseBindSocketToBr0$|acdsRcvApAutoConfigSearch$|_acdcRcvApAutoConfig|_acdsRcvApAutoConfig)", name)}}
        (work / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        (work / "vendor-abi.json").write_text(json.dumps(abi, indent=2, sort_keys=True) + "\n")
        runtime.rename(output / "runtime")
        (work / "manifest.json").replace(output / "manifest.json")
        (work / "vendor-abi.json").replace(output / "vendor-abi.json")
        return manifest


def prepare_analysis_cspd(rootfs: Path, output: Path, unsquashfs: str) -> Path:
    """Extract a non-executable static evidence copy; never add cspd to runtime."""
    analysis = output / "analysis"
    destination = analysis / "cspd"
    if analysis.is_symlink() or destination.is_symlink():
        raise EvidenceError("analysis evidence must not be a symlink")
    analysis.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        checked(destination.read_bytes(), CSPD_SHA256, "analysis cspd")
        destination.chmod(0o600)
        return destination
    image = rootfs.read_bytes()
    checked(image, ROOTFS_SHA256, "factory rootfs")
    nested = extract_file(image, "kmoduletmp/kmodule.img")
    checked(nested, KMODULE_SHA256, "nested kmodule image")
    with tempfile.TemporaryDirectory(prefix=".mesh-analysis-", dir=output) as work:
        work = Path(work)
        (work / "kmodule.img").write_bytes(nested)
        result = subprocess.run([unsquashfs, "-no-progress", "-processors", "1", "-d",
                                 str(work / "extracted"), str(work / "kmodule.img"), "bin/cspd"],
                                capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise EvidenceError(f"analysis unsquashfs failed: {result.stderr[-1500:]}")
        source = work / "extracted/bin/cspd"
        if source.is_symlink() or not source.is_file():
            raise EvidenceError("analysis cspd is not a regular file")
        data = source.read_bytes()
        checked(data, CSPD_SHA256, "analysis cspd")
        ELF(data).require_vendor_abi()
        staged = work / "cspd.evidence"
        staged.write_bytes(data)
        staged.chmod(0o600)
        staged.replace(destination)
    return destination


def prepare_client_lib(rootfs: Path, output: Path, unsquashfs: str) -> Path:
    """Pinned liboss_pub.so for the factory-style OSS client helper. Kept outside
    runtime/ and the daemon's library closure: it must never be linked with liboss."""
    destination = output / "runtime-extra/lib/liboss_pub.so"
    if destination.is_symlink() or (output / "runtime-extra").is_symlink():
        raise EvidenceError("client library path must not be a symlink")
    if destination.exists():
        checked(destination.read_bytes(), LIBOSS_PUB_SHA256, "client liboss_pub")
        return destination
    image = rootfs.read_bytes()
    checked(image, ROOTFS_SHA256, "factory rootfs")
    nested = extract_file(image, "kmoduletmp/kmodule.img")
    checked(nested, KMODULE_SHA256, "nested kmodule image")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".mesh-client-", dir=output) as work:
        work = Path(work)
        (work / "kmodule.img").write_bytes(nested)
        result = subprocess.run([unsquashfs, "-no-progress", "-processors", "1", "-d", str(work / "x"),
                                 str(work / "kmodule.img"), "lib/liboss_pub.so"],
                                capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise EvidenceError(f"client unsquashfs failed: {result.stderr[-1500:]}")
        source = work / "x/lib/liboss_pub.so"
        if source.is_symlink() or not source.is_file():
            raise EvidenceError("client library is not a regular file")
        data = source.read_bytes()
        checked(data, LIBOSS_PUB_SHA256, "client liboss_pub")
        ELF(data).require_vendor_abi()
        staged = work / "liboss_pub.so"
        staged.write_bytes(data)
        staged.chmod(0o600)
        staged.replace(destination)
    return destination


def main():
    repo = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    # The factory image stays in the kernel project; SR1010_VENDOR_ROOTFS overrides the location.
    rootfs = os.environ.get("SR1010_VENDOR_ROOTFS", str(repo.parent / "zx279133/vendor-reference/2b5/rootfs.jffs2"))
    parser.add_argument("--rootfs", type=Path, default=Path(rootfs))
    parser.add_argument("--output", type=Path, default=repo / "out")
    parser.add_argument("--unsquashfs", default=shutil.which("unsquashfs"))
    parser.add_argument("--analysis-cspd", action="store_true", help="extract pinned static evidence outside runtime")
    args = parser.parse_args()
    if not args.unsquashfs:
        parser.error("unsquashfs is required (squashfs-tools)")
    try:
        manifest = prepare(args.rootfs, args.output.absolute(), args.unsquashfs)
        if args.analysis_cspd:
            prepare_analysis_cspd(args.rootfs, args.output.absolute(), args.unsquashfs)
        prepare_client_lib(args.rootfs, args.output.absolute(), args.unsquashfs)
    except (EvidenceError, ELFError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"prepare failed: {error}\n")
    print(f"Prepared {args.output}/runtime: {manifest['library_count']} libraries, verified hashes, symbols and version requirements")


if __name__ == "__main__":
    main()
