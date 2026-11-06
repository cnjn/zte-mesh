#!/usr/bin/env python3
"""Static poking at the pinned vendor binaries, using their symbol table.

multiapd ships unstripped, so the whole question "why did the agent build an M1
and never send it" is answerable without running anything: find the builder,
find who is supposed to call the sender, and read the flag that gates it.

  python3 tools/lab/elfx.py syms M1                 # names matching a regex
  python3 tools/lab/elfx.py callers acdcBuildM1Msg  # who calls it
  python3 tools/lab/elfx.py calls   acdcBuildM1Msg  # what it calls
  python3 tools/lab/elfx.py dis     acdcBuildM1Msg  # disassembly (ARM + literal pool)
  python3 tools/lab/elfx.py xref    0x89e1c         # everything that touches an address

The disassembler is capstone, so it is a dev-machine tool (`pip install capstone`);
`syms`/`callers`/`calls`/`xref` only need the ELF parser below.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

SHDR = struct.Struct("<IIIIIIIIII")
SYM = struct.Struct("<IIIBBH")
REL = struct.Struct("<II")


class Elf:
    """ELF32 little-endian, just the parts these binaries need."""

    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        shoff = struct.unpack_from("<I", d, 0x20)[0]
        shents = struct.unpack_from("<H", d, 0x2E)[0]
        shnum = struct.unpack_from("<H", d, 0x30)[0]
        shstrndx = struct.unpack_from("<H", d, 0x32)[0]
        self.sh = [SHDR.unpack_from(d, shoff + i * shents) for i in range(shnum)]
        self.names = {}
        for i, s in enumerate(self.sh):
            n = self._str(s[0], self.sh[shstrndx][4])
            if n:
                self.names[n] = i
        self.funcs = []
        self.by_name = {}
        self._symbols()
        self._plt()
        self._callgraph()

    def _str(self, off, table):
        if not off:
            return ""
        s = table + off
        return self.data[s:self.data.index(b"\0", s)].decode("utf-8", "replace")

    def sec(self, name):
        return self.sh[self.names[name]]

    def has(self, name):
        return name in self.names

    def _symbols(self):
        if not self.has(".symtab"):
            return
        sym = self.sec(".symtab")
        strtab = self.sh[sym[6]][4]
        out = []
        for i in range(sym[5] // SYM.size):
            name, val, size, info, _o, shndx = SYM.unpack_from(self.data, sym[4] + i * SYM.size)
            if (info & 0xF) == 2 and val and size and shndx:
                out.append((val, size, self._str(name, strtab)))
        out.sort()
        self.funcs = out
        self.by_name = {n: (v, s, n) for v, s, n in out}

    def _plt(self):
        self.plt_addr = self.sec(".plt")[3] if self.has(".plt") else 0
        dyn = self.sec(".dynsym")
        dstr = self.sh[dyn[6]][4]
        self.dynsyms = []
        for i in range(dyn[5] // SYM.size):
            name = SYM.unpack_from(self.data, dyn[4] + i * SYM.size)[0]
            self.dynsyms.append(self._str(name, dstr) if name else "")
        self.rel = self.sec(".rel.plt") if self.has(".rel.plt") else None
        self.plt_count = self.rel[5] // REL.size if self.rel else 0
        self.exports = set()
        for i in range(dyn[5] // SYM.size):
            _n, _v, _s, _i, _o, shndx = SYM.unpack_from(self.data, dyn[4] + i * SYM.size)
            if shndx:
                self.exports.add(self.dynsyms[i] or "")

    def plt_name(self, addr):
        if not self.plt_addr:
            return None
        idx = (addr - self.plt_addr - 20) // 12
        if 0 <= idx < self.plt_count:
            _off, info = REL.unpack_from(self.data, self.rel[4] + idx * REL.size)
            sym = info >> 8
            if sym < len(self.dynsyms):
                return self.dynsyms[sym] or None
        return None

    def offset(self, addr):
        text = self.sec(".text")
        return text[4] + (addr - text[3])

    def read32(self, addr):
        try:
            return struct.unpack_from("<I", self.data, self.offset(addr))[0]
        except (struct.error, ValueError):
            return None

    def func_at(self, addr):
        lo, hi, best = 0, len(self.funcs) - 1, None
        while lo <= hi:
            mid = (lo + hi) // 2
            if self.funcs[mid][0] <= addr:
                best, lo = self.funcs[mid], mid + 1
            else:
                hi = mid - 1
        if not best or not (best[0] <= addr < best[0] + best[1]):
            return None
        return best

    def _decode(self, fn):
        """(offset, target) for every ARM branch-with-link inside a function."""
        start, size, _n = fn
        base = self.offset(start)
        for i in range(0, max(0, size - 3), 4):
            word = struct.unpack_from("<I", self.data, base + i)[0]
            if (word >> 28) != 0xE or ((word >> 24) & 0xF) not in (0xA, 0xB):
                continue
            imm = word & 0xFFFFFF
            if imm & 0x800000:
                imm -= 0x1000000
            yield i, start + i + 8 + imm * 4

    def name_of(self, target):
        n = self.plt_name(target)
        if n:
            return n + " [plt]"
        f = self.func_at(target)
        if f and f[0] == target:
            return f[2]
        return f"0x{target:x}"

    def calls(self, fn):
        seen = {}
        for _i, t in self._decode(fn):
            k = self.name_of(t)
            seen[k] = seen.get(k, 0) + 1
        return dict(sorted(seen.items(), key=lambda kv: -kv[1]))

    def callers(self, target_name):
        t = self.by_name.get(target_name)
        if not t:
            hits = [f for f in self.funcs if target_name in f[2]]
            if len(hits) == 1:
                t = hits[0]
            else:
                raise SystemExit(f"no unique symbol {target_name}")
        out = []
        for fn in self.funcs:
            for off, dest in self._decode(fn):
                if dest == t[0]:
                    out.append((fn[0] + off, fn))
        return out

    def _callgraph(self):
        self.callers_by_addr = {}
        for fn in self.funcs:
            for off, dest in self._decode(fn):
                self.callers_by_addr.setdefault(dest, []).append((fn[0] + off, fn))

    def who_calls_addr(self, addr):
        return self.callers_by_addr.get(addr, [])

    def literals(self, fn):
        """(addr, value) for every ldr rX,[pc,#imm] inside a function."""
        start, size, _n = fn
        base = self.offset(start)
        for i in range(0, max(0, size - 3), 4):
            word = struct.unpack_from("<I", self.data, base + i)[0]
            if (word & 0x0F7F0000) == 0x051F0000:  # ldr rt,[pc,#+/-imm]
                lit = start + i + 8 + (word & 0xFFF) * (-1 if word & 0x00800000 else 1)
                v = self.read32(lit)
                if v is not None:
                    yield start + i, word, lit, v


def find_elf(repo, explicit):
    if explicit:
        return Path(explicit)
    for cand in ("out/vendor/root/bin/multiapd", "out/runtime/bin/multiapd"):
        p = repo / cand
        if p.exists():
            return p
    raise SystemExit("no multiapd; pass --elf")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["syms", "callers", "calls", "dis", "xref", "literals"])
    ap.add_argument("what", help="symbol name, regex (syms) or hex address (xref)")
    ap.add_argument("--elf", default=None)
    args = ap.parse_args()

    repo = Path(__file__).resolve().parents[2]
    path = find_elf(repo, args.elf)
    elf = Elf(path)

    if args.command == "syms":
        rx = re.compile(args.what, re.I)
        for v, s, n in elf.funcs:
            if rx.search(n):
                print(f"0x{v:08x} {s:7d} {n}")
        return

    if args.command == "callers":
        hits = elf.callers(args.what)
        if not hits:
            print("no direct callers")
        for at, fn in hits:
            print(f"0x{at:08x} in {fn[2]} @0x{fn[0]:x}")
        return

    if args.command == "xref":
        addr = int(args.what, 0)
        print(f"== call to 0x{addr:x} ({elf.name_of(addr)})")
        for at, fn in elf.who_calls_addr(addr):
            print(f"  0x{at:08x} in {fn[2]}")
        print("== literal pools holding 0x%08x" % addr)
        for fn in elf.funcs:
            for at, word, lit, v in elf.literals(fn):
                if v == addr:
                    print(f"  0x{at:08x} in {fn[2]} (pool 0x{lit:08x})")
        return

    fn = elf.by_name.get(args.what)
    if not fn:
        raise SystemExit(f"no symbol {args.what}")

    if args.command == "calls":
        print(f"{fn[2]} @0x{fn[0]:x} ({fn[1]} bytes)")
        for k, c in elf.calls(fn).items():
            print(f"  {c:3d}x {k}")
        return

    if args.command == "literals":
        for at, word, lit, v in elf.literals(fn):
            print(f"0x{at:08x} ldr  [0x{lit:08x}] = 0x{v:08x}")
        return

    if args.command == "xref":
        addr = int(args.what, 0)
        print(f"== call to 0x{addr:x} ({elf.name_of(addr)})")
        for at, fn in elf.who_calls_addr(addr):
            print(f"  0x{at:08x} in {fn[2]}")
        print("== literal pools holding 0x%08x" % addr)
        for fn in elf.funcs:
            for at, word, lit, v in elf.literals(fn):
                if v == addr:
                    print(f"  0x{at:08x} in {fn[2]} (pool 0x{lit:08x})")
        return

    from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    start, size, name = fn
    base = elf.offset(start)
    text = elf.data[base:base + size]
    pool = {}
    for at, word, lit, v in elf.literals(fn):
        pool[lit] = v
    pending = []
    for ins in md.disasm(text, start):
        note = ""
        for at, word, lit, v in elf.literals(fn):
            if at == ins.address:
                pending.append(lit)
        body = f"{ins.address:08x}  {ins.mnemonic:8s} {ins.op_str}"
        if ins.mnemonic.startswith("bl") or ins.mnemonic.startswith("b") and ins.op_str.startswith("#"):
            try:
                t = int(ins.op_str.lstrip("#"), 0)
                body += f"        ; {elf.name_of(t)}"
            except ValueError:
                pass
        print(body)
    if pool:
        print("-- literal pool")
        for lit in sorted(pool):
            print(f"  0x{lit:08x} = 0x{pool[lit]:08x}  {elf.name_of(pool[lit]) if pool[lit] else ''}")


if __name__ == "__main__":
    main()
