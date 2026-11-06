#!/usr/bin/env python3
"""Who depends on whom: DT_NEEDED / SONAME / dynsym 供求关系统计（ARM32，无 binutils）。

与 `syms.py` 的分工：`syms.py` 答"这个符号在哪导出/被谁导入"（单文件视角）；
这里答"一个可执行文件的全部导入，逐个回溯到目录里哪个 `.so` 供的货"——
也就是判断"这套厂商栈是不是闭包"时真正需要的那个数。见
`docs/research/BLOB_SHIM.md` §2（multiapd：220/225 由随包 .so 导出，内核 0）。

用法:
    python3 tools/lab/elfdeps.py <elf> [...]        # 单个文件的 NEEDED/SONAME/符号计数
    python3 tools/lab/elfdeps.py --graph <dir>      # 扫目录内所有 ELF，列 soname 与 NEEDED
"""
import struct
import sys
from pathlib import Path

DT_NEEDED, DT_SONAME = 1, 14


def parse(path):
    d = Path(path).read_bytes()
    if d[:4] != b"\x7fELF":
        raise ValueError("not ELF: %s" % path)
    cls = d[4]
    end = "<" if d[5] == 1 else ">"
    if cls != 1:
        raise ValueError("not ELF32: %s" % path)
    (_, machine, _, _, _, shoff, _, _, _, _, shentsize, shnum,
     shstrndx) = struct.unpack(end + "HHIIIIIHHHHHH", d[16:52])
    secs = []
    for i in range(shnum):
        off = shoff + i * shentsize
        name, typ, flags, addr, offset, size, link, info, align, ent = \
            struct.unpack(end + "IIIIIIIIII", d[off:off + 40])
        secs.append(dict(name_off=name, type=typ, addr=addr, off=offset, size=size,
                         link=link, ent=ent))
    shstr = secs[shstrndx]
    base = shstr["off"]
    for s in secs:
        e = d.index(b"\x00", base + s["name_off"])
        s["name"] = d[base + s["name_off"]:e].decode("latin1")

    by_name = {s["name"]: s for s in secs}

    # .dynamic -> NEEDED / SONAME
    needed, soname = [], None
    if ".dynamic" in by_name:
        s = by_name[".dynamic"]
        n = s["size"] // 8
        tags = []
        for i in range(n):
            tag, val = struct.unpack(end + "iI", d[s["off"] + i * 8: s["off"] + i * 8 + 8])
            if tag == 0:
                break
            tags.append((tag, val))
        dynstr = None
        if ".dynstr" in by_name:
            ds = by_name[".dynstr"]
            dynstr = d[ds["off"]: ds["off"] + ds["size"]]

        def dstr(v):
            e = dynstr.index(b"\x00", v)
            return dynstr[v:e].decode("latin1")

        for tag, val in tags:
            if not dynstr:
                break
            if tag == DT_NEEDED:
                needed.append(dstr(val))
            elif tag == DT_SONAME:
                soname = dstr(val)

    # .dynsym 供求
    defs, undef = set(), set()
    if ".dynsym" in by_name:
        sym, st = by_name[".dynsym"], by_name[".dynstr"]
        strtab = d[st["off"]: st["off"] + st["size"]]
        n = int(sym["size"] / 16) if sym["ent"] == 0 else int(sym["size"] / sym["ent"])
        for i in range(n):
            o = sym["off"] + i * 16
            name_off, value, size, info, other, shndx = struct.unpack(
                end + "IIIBBH", d[o:o + 16])
            if name_off == 0:
                continue
            e = strtab.index(b"\x00", name_off)
            nm = strtab[name_off:e].decode("latin1")
            if shndx == 0:
                undef.add(nm)
            else:
                defs.add(nm)
    return dict(path=str(path), machine=machine, needed=needed, soname=soname,
                defined=defs, undefined=undef)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    if args[0] == "--closure":
        # 一个可执行的全部导入，逐个回溯到 libdir 里哪个 .so 供的货。
        # 这就是 docs/research/BLOB_SHIM.md §2 那张表的生成方式。
        exe, libdir = Path(args[1]), Path(args[2])
        prov = {}
        for p in sorted(libdir.glob("*.so*")):
            if p.stat().st_size < 4096:
                continue
            try:
                e = parse(p)
            except Exception:
                continue
            for s in e["defined"]:
                prov.setdefault(s, p.name)
        m = parse(exe)
        cnt, missing = {}, []
        for s in m["undefined"]:
            k = prov.get(s)
            if k is None:
                missing.append(s)
            else:
                cnt[k] = cnt.get(k, 0) + 1
        print("%s: NEEDED %d，导入 %d" % (exe.name, len(m["needed"]), len(m["undefined"])))
        for k, v in sorted(cnt.items(), key=lambda kv: -kv[1]):
            print("  %-28s %3d" % (k, v))
        print("  %-28s %3d" % ("(无 .so 供货)", len(missing)))
        if missing:
            print("    " + " ".join(sorted(missing)))
        return

    if args[0] == "--graph":
        root = Path(args[1])
        nodes = {}
        for p in sorted(root.rglob("*")):
            if not p.is_file() or p.stat().st_size < 4096:
                continue
            try:
                nodes[p] = parse(p)
            except Exception:
                continue
        print("解析 %d 个 ELF" % len(nodes))
        for p, e in sorted(nodes.items(), key=lambda kv: str(kv[0])):
            rel = str(p.relative_to(root))
            print("\n%-24s soname=%-22s needed=%s" %
                  (rel, e["soname"] or "-", ",".join(e["needed"]) or "-"))
        return

    for a in args:
        e = parse(a)
        print("== %s" % a)
        print("   machine=0x%04x  soname=%s" % (e["machine"], e["soname"]))
        print("   NEEDED      = %s" % (",".join(e["needed"]) or "-"))
        print("   defined=%d  undefined=%d" % (len(e["defined"]), len(e["undefined"])))
        if "-v" in args:
            for s in sorted(e["undefined"]):
                print("     U %s" % s)


if __name__ == "__main__":
    main()
