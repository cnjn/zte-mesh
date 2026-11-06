#!/usr/bin/env python3
"""Print the syscall-facing exports/imports of the factory libraries.

    python3 tools/lab/syms.py out/runtime/lib/libc.so.6 --prefix shm sem
    python3 tools/lab/syms.py out/runtime/bin/multiapd --imports

Kept separate from the build so the question "which object owns shmat()" can be
answered without re-deriving the ELF tables by hand.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "dev"))

from vendor_elf import ELF  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--prefix", nargs="*", default=["shm", "sem"])
    parser.add_argument("--imports", action="store_true", help="show imports instead of exports")
    args = parser.parse_args()

    for path in args.paths:
        elf = ELF.read(path)
        names = elf.imports if args.imports else elf.exports
        selected = sorted(n for n in names if n.startswith(tuple(args.prefix)))
        print(f"{path}  ({'imports' if args.imports else 'exports'})")
        print("   " + " ".join(selected) if selected else "   (none)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
