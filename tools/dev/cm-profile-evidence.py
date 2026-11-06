#!/usr/bin/env python3
"""Recover read-only AP profile copies from pinned ARM machine instructions.

Only metadata/instructions are read. No factory credential/default pointers.
The CM AP profile is 2496 bytes, not the 2312-byte WLANCfg database view.
"""
import argparse
import json
import struct
from pathlib import Path

from profile_schema import registry
from schema import Image

CM_SHA256 = "e19109de13f6eb7ccc344026e04711c971965d8fdc1af0c28d7cfb1ee8272d36"
AP_BYTES = 2496
# Actual query passed to WlanCfgDbRead lives at fp-2348.
DB_STACK = -2348
# Hash-pinned copies through oss_strncpy_s: callsite, payload destination,
# named source payload, capacity including the native four-byte reserve.
STRINGS = [(0x141004, 124, "Alias", 68), (0x141038, 220, "IfName", 68),
           (0x141074, 1468, "SSIDReference", 260),
           (0x14111c, 444, "ESSID", 36), (0x141154, 508, "ESSIDPrefix", 36),
           (0x1413d0, 1084, "WPAEAPSecret", 68)]


def word(image, pc):
    return struct.unpack("<I", image.read(pc, 4))[0]


def branch_target(pc, insn):
    displacement = insn & 0xffffff
    if displacement & 0x800000:
        displacement -= 0x1000000
    return pc + 8 + displacement * 4


def string_arguments(image):
    # Symbolic addresses only, over the straight-line profile copy block.
    # We never dereference a native source string or arbitrary pointer.
    regs = [None] * 16
    regs[4], regs[11] = ("out", 0), ("stack", 0)
    recovered = {}
    def operand(insn):
        if insn & (1 << 25):
            v, shift = insn & 255, ((insn >> 8) & 15) * 2
            return ((v >> shift) | (v << (32 - shift))) & 0xffffffff if shift else v
        return regs[insn & 15] if insn & 0xff0 == 0 else None
    def add(a, b):
        if isinstance(a, tuple) and isinstance(b, int):
            return a[0], a[1] + b
        if isinstance(a, int) and isinstance(b, int):
            return a + b
        return None
    for pc in range(0x140fa4, 0x1413d4, 4):
        insn = word(image, pc)
        rd, rn = (insn >> 12) & 15, (insn >> 16) & 15
        if insn & 0xff000000 == 0xeb000000:
            if branch_target(pc, insn) == 0x1d9b8:
                recovered[pc] = tuple(regs[:4])
            regs[:4] = [None] * 4
        elif insn & 0x0ff00000 == 0x03000000:
            regs[rd] = (insn & 0xfff) | ((insn >> 4) & 0xf000)
        elif insn & 0x0c100000 == 0x04100000:
            address = add(regs[rn], (insn & 0xfff) * (1 if insn & (1 << 23) else -1))
            regs[rd] = ("stack", -2340) if address == ("stack", -2884) else None
        elif insn & 0x0c000000 == 0:
            op, rhs = (insn >> 21) & 15, operand(insn)
            if op == 13:
                regs[rd] = rhs
            elif op in (2, 4):
                regs[rd] = add(regs[rn], -rhs if op == 2 and isinstance(rhs, int) else rhs) if op == 4 or isinstance(rhs, int) else None
    return recovered


def recover(path):
    image = Image(path, CM_SHA256)
    fields = registry("WLANCfg")["fields"]
    by_value = {f["value_offset"]: f for f in fields}
    by_name = {f["name"]: f for f in fields}
    copies = []
    # Straight-line scalar copy region. Only matching, understood pairs are
    # accepted; source must be the beginning of a recovered field payload.
    for pc in range(0x140fc8, 0x1413ec, 4):
        load, store = word(image, pc), word(image, pc + 4)
        width = 1 if load & 0xfffff000 == 0xe55b3000 else 4
        if load & 0xfffff000 not in (0xe55b3000, 0xe51b3000):
            continue
        wanted = 0xe5c43000 if width == 1 else 0xe5843000
        gap = 0
        if store & 0xfffff000 != wanted:
            # The compiler sometimes slots one independent load of another register (r0-r2, from the frame) between the
            # load of r3 and its store; that is still the same straight-line copy of one scalar.
            between = store
            store = word(image, pc + 8)
            if between & 0xfffff000 not in (0xe51b0000, 0xe51b1000, 0xe51b2000) or store & 0xfffff000 != wanted:
                continue
            gap = 4
        source = -(load & 0xfff) - DB_STACK
        dest = store & 0xfff
        field = by_value.get(source)
        if field is None or field["type"] < 12 or field["value_length"] != width:
            raise ValueError("unrecognized scalar source/type")
        copies.append({"pc": pc + gap, "source": source, "dest": dest, "bytes": width,
                       "name": field["name"], "type": field["type"]})
    if len(copies) != 38:
        raise ValueError(f"scalar copy count changed: {len(copies)}")
    arguments = string_arguments(image)
    for pc, dest, name, capacity in STRINGS:
        if word(image, pc) & 0xff000000 != 0xeb000000 or branch_target(pc, word(image, pc)) != 0x1d9b8:
            raise ValueError("native string copy call changed")
        f = by_name[name]
        if (f["type"] >= 12 or f["value_length"] != capacity or
                arguments.get(pc) != (("out", dest), capacity,
                                      ("stack", DB_STACK + f["value_offset"]), capacity - 4)):
            raise ValueError("native string arguments/capacity/type changed")
        copies.append({"pc": pc, "source": f["value_offset"], "dest": dest,
                       "bytes": capacity, "name": name, "type": f["type"]})
    # Do not infer output size from our adapter: verify a actual caller's
    # 2496-byte initialization separately via caller evidence in verification.
    occupied = set()
    for c in copies:
        span = set(range(c["dest"], c["dest"] + c["bytes"]))
        if c["dest"] < 28 or c["dest"] + c["bytes"] > AP_BYTES or span & occupied:
            raise ValueError("overlapping/out-of-range CM payload copies")
        occupied |= span
    return sorted(copies, key=lambda c: c["dest"])


def render(copies):
    lines = ["/* Generated from pinned libcmapi.so CmWLANGetAPConfig instructions.",
             " * Source defaults/credentials were not read. PUBLIC metadata only.",
             f" * SHA256: {CM_SHA256}", " */"]
    for c in copies:
        lines.append("CM_COPY(%d, %d, %d, %d) /* %s @0x%x */" %
                     (c["source"], c["dest"], c["bytes"], c["type"], c["name"], c["pc"]))
    return "\n".join(lines) + "\n"


def verify_caller(path):
    from prepare import MULTIAPD_SHA256
    image = Image(path, MULTIAPD_SHA256)
    # get_bssInfo_from_wlancfg: mov r3,#2496 before oss_memset_s.
    if word(image, 0x49d08) != 0xe3a03d27 or word(image, 0x44cf0) != 0xe3a03f51 or word(image, 0x44da4) != 0xb4f5e:
        raise ValueError("CM AP/PSK caller-size evidence changed")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--libcmapi", type=Path, required=True)
    p.add_argument("--multiapd", type=Path)
    p.add_argument("--emit", type=Path)
    p.add_argument("--verify", type=Path)
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    copies = recover(args.libcmapi)
    if args.multiapd:
        verify_caller(args.multiapd)
    text = render(copies)
    if args.emit:
        with args.emit.open("x") as f:
            f.write(text)
    if args.verify and args.verify.read_text() != text:
        raise ValueError("CM profile copy registry differs from pinned machine code")
    if args.output:
        args.output.write_text(json.dumps({"source_sha256": CM_SHA256, "ap_bytes": AP_BYTES,
                                           "copies": copies}, indent=2, sort_keys=True) + "\n")
    print(f"CM AP profile evidence: {len(copies)} payload copies, {AP_BYTES} bytes; metadata only")


if __name__ == "__main__":
    main()
