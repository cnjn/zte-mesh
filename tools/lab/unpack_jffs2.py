#!/usr/bin/env python3
"""unpack_jffs2.py -- 纯标准库的 JFFS2 解开器（无 jefferson / mtd-utils 时用）

用法:
    python tools/lab/unpack_jffs2.py rootfs.jffs2 -o out/          # 解到目录
    python tools/lab/unpack_jffs2.py rootfs.jffs2 --list           # 只列清单与统计

为什么要自己写: ZTE 那台机器的 busybox 没有 dd，取回来的镜像是用 `cat /dev/mtdblock8`
拿的；而本机 pip 装不了 jefferson、也没有 mtd-utils。JFFS2 节点格式简单，标准库足够。

格式（全部大端）:
  unknown_node { u16 magic=0x1985; u16 nodetype; u32 totlen; u32 hdr_crc }   # 12 字节
  raw_inode    { node; u32 version, ino, mode; u16 uid, gid; u32 isize;
                 u32 atime, mtime, ctime, offset; u32 csize, dsize;
                 u8 compr, usercompr; u16 flags; u32 data_crc }              # 68 字节
  raw_dirent   { node; u32 pino, version, ino, mctime; u8 nsize, type;
                 u16 unused; u32 node_crc, name_crc; u8 name[nsize] }        # 40+nsize

损坏或已擦除的区域按 4 字节步进跳过；inode 数据按 (ino, offset) 取最高 version。
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib
from collections import Counter, defaultdict

MAGIC = 0x1985

JFFS2_NODETYPE_DIRENT = 0xE001
JFFS2_NODETYPE_INODE = 0xE002
JFFS2_NODETYPE_CLEANMARKER = 0xE003
JFFS2_NODETYPE_PADDING = 0xE004
JFFS2_NODETYPE_SUMMARY = 0xE008
JFFS2_NODETYPE_XATTR = 0xE009
JFFS2_NODETYPE_XREF = 0xE00A

NODE_NAMES = {
    JFFS2_NODETYPE_DIRENT: "dirent",
    JFFS2_NODETYPE_INODE: "inode",
    JFFS2_NODETYPE_CLEANMARKER: "cleanmarker",
    JFFS2_NODETYPE_PADDING: "padding",
    JFFS2_NODETYPE_SUMMARY: "summary",
    JFFS2_NODETYPE_XATTR: "xattr",
    JFFS2_NODETYPE_XREF: "xref",
}

COMPR_NONE = 0x00
COMPR_ZERO = 0x01
COMPR_RTIME = 0x02
COMPR_RUBINMIPS = 0x03
COMPR_COPY = 0x04
COMPR_DYNRUBIN = 0x05
COMPR_ZLIB = 0x06
COMPR_LZO = 0x07
COMPR_LZMA = 0x08
COMPR_ZSTD = 0x09

COMPR_NAMES = {
    COMPR_NONE: "none", COMPR_ZERO: "zero", COMPR_RTIME: "rtime",
    COMPR_RUBINMIPS: "rubinmips", COMPR_COPY: "copy", COMPR_DYNRUBIN: "dynrubin",
    COMPR_ZLIB: "zlib", COMPR_LZO: "lzo", COMPR_LZMA: "lzma", COMPR_ZSTD: "zstd",
}

DT = {0: "---", 1: "fifo", 2: "chr", 4: "dir", 6: "blk", 8: "reg", 10: "lnk", 12: "sock"}

ROOT_INO = 1


_CRC_TABLE: list[int] = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if _c & 1 else _c >> 1
    _CRC_TABLE.append(_c)


def crc32(data: bytes, crc: int = 0) -> int:
    """JFFS2 的 crc32: reflected poly 0xEDB88320，init=0，**无收尾异或**。

    注意：这正是 `zlib.crc32(data) ^ 0xFFFFFFFF` 之外的那个变体——直接用 zlib
    会把 6412 个节点的 hdr_crc 全部判成坏节点（踩过）。
    """
    for b in data:
        crc = (crc >> 8) ^ _CRC_TABLE[(crc ^ b) & 0xFF]
    return crc & 0xFFFFFFFF


# ---------------------------------------------------------------- decompressors


def dec_rtime(src: bytes, dsize: int) -> bytes:
    """jffs2_rtime_decompress 的逐字节移植：值 + 回退长度 两字节一组。"""
    positions = [0] * 256
    out = bytearray()
    pos = 0
    n = len(src)
    while len(out) < dsize:
        if pos + 2 > n:
            raise ValueError("rtime: truncated")
        value = src[pos]
        repeat = src[pos + 1]
        pos += 2
        out.append(value)
        backoffs = positions[value]
        positions[value] = len(out)
        if repeat:
            for _ in range(repeat):
                out.append(out[backoffs])
                backoffs += 1
    return bytes(out[:dsize])


def decompress(compr: int, src: bytes, dsize: int) -> bytes:
    if compr in (COMPR_NONE, COMPR_COPY):
        return src[:dsize]
    if compr == COMPR_ZERO:
        return b"\x00" * dsize
    if compr == COMPR_RTIME:
        return dec_rtime(src, dsize)
    if compr == COMPR_ZLIB:
        return zlib.decompress(src)[:dsize]
    if compr == COMPR_LZMA:
        import lzma  # noqa: PLC0415

        return lzma.decompress(src)[:dsize]
    raise ValueError(f"unsupported compression {COMPR_NAMES.get(compr, hex(compr))}")


# ---------------------------------------------------------------- node walking


class Stats:
    def __init__(self) -> None:
        self.nodes = Counter()
        self.compr = Counter()
        self.bad_hdr_crc = 0
        self.bad_data_crc = 0
        self.skipped = 0
        self.dec_fail = Counter()
        self.endian = "?"
        self.layout = "?"
        self.layout_votes: dict[str, int] = {}


# raw_inode 的两种布局。字节位置相对**节点起点**（node 头 12 字节之后）。
#   标准 jffs2 (mtd-utils / jefferson / 原厂多数机型): 头 64 B，data 紧跟 data_crc
#   本机实测 (E2633/E2631 这一类):                     头 68 B，data_crc 后多一个 u32
# 两者的 mode/isize/offset/csize/dsize/compr 位置完全相同，只差 data 起点，
# 所以在数据节点上用 data_crc 当判据自动选一个即可。
LAYOUTS = (
    {"name": "std64", "data_crc": 60, "data": 64},
    {"name": "zte68", "data_crc": 60, "data": 68},
)


def _inode_blob(data: bytes, off: int, totlen: int, E: str, layout: dict):
    hdr = layout["data"]
    body = data[off:off + totlen]
    if len(body) < hdr:
        return None
    isize = struct.unpack_from(E + "I", body, 28)[0]
    foffset, csize, dsize = struct.unpack_from(E + "III", body, 44)
    if hdr + csize > totlen:
        return None
    return isize, foffset, csize, dsize, body[56], struct.unpack_from(
        E + "I", body, layout["data_crc"]
    )[0], body[hdr:hdr + csize]


def pick_layout(data: bytes, E: str, stats: Stats) -> dict:
    """用前若干数据节点的 data_crc 投票选 raw_inode 布局。"""
    votes = {lay["name"]: 0 for lay in LAYOUTS}
    off, n, seen = 0, len(data), 0
    while off + 12 <= n and seen < 48:
        magic, nodetype, totlen, _ = struct.unpack_from(E + "HHII", data, off)
        if magic == MAGIC and 12 <= totlen <= n - off and nodetype == JFFS2_NODETYPE_INODE:
            for lay in LAYOUTS:
                got = _inode_blob(data, off, totlen, E, lay)
                if not got:
                    continue
                _, _, csize, dsize, _, data_crc, blob = got
                if csize and dsize and crc32(blob) == data_crc:
                    votes[lay["name"]] += 1
            seen += 1
            off += (totlen + 3) & ~3
        else:
            off += 4
    best = max(LAYOUTS, key=lambda lay: votes[lay["name"]])
    stats.layout = best["name"]
    stats.layout_votes = dict(votes)
    return best


def walk(data: bytes, stats: Stats):
    """产出 (offset, nodetype, payload_fields)。坏区域按 4 字节步进。

    字节序: JFFS2 按写盘主机序存储。magic 常量是 0x1985，所以磁盘首两字节
    `85 19` 表示小端（ARM 设备产出的一般如此），`19 85` 表示大端（MIPS/PPC 老货）。
    magic 判定之后所有 u16/u32 都用同一序解。

    CRC 用 JFFS2 自己的变体（见 crc32()），不是 zlib 的。
    """
    if data[:2] == b"\x85\x19":
        E = "<"
    elif data[:2] == b"\x19\x85":
        E = ">"
    else:
        raise SystemExit("not a JFFS2 image (magic != 0x1985)")
    stats.endian = "little" if E == "<" else "big"
    layout = pick_layout(data, E, stats)

    off = 0
    n = len(data)
    while off + 12 <= n:
        magic, nodetype, totlen, hdr_crc = struct.unpack_from(E + "HHII", data, off)
        ok = (
            magic == MAGIC
            and 12 <= totlen <= n - off
            and (nodetype & 0xE000) == 0xE000  # 节点类型都在 0xExxx
        )
        if not ok:
            stats.skipped += 1
            off += 4
            continue
        if crc32(data[off:off + 8]) != hdr_crc:
            stats.bad_hdr_crc += 1
            off += 4
            continue

        stats.nodes[NODE_NAMES.get(nodetype, hex(nodetype))] += 1
        body = data[off + 12:off + totlen]

        if nodetype == JFFS2_NODETYPE_DIRENT and len(body) >= 28:
            pino, version, ino, mctime = struct.unpack_from(E + "IIII", body, 0)
            nsize = body[16]
            dtype = body[17]
            name = body[28:28 + nsize].decode("utf-8", "replace")
            yield ("dirent", pino, version, ino, dtype, name)
        elif nodetype == JFFS2_NODETYPE_INODE:
            got = _inode_blob(data, off, totlen, E, layout)
            if got is None:
                yield ("other", nodetype, totlen)
            else:
                isize, foffset, csize, dsize, compr, data_crc, blob = got
                # 这个 body 是**跳过 12 字节 node 头**之后的，所以 ino 在 0
                # （节点内布局: ino@12 version@16 mode@20 uid@24 gid@26 ...）
                ino, version, mode = struct.unpack_from(E + "III", body, 0)
                uid, gid = struct.unpack_from(E + "HH", body, 12)
                stats.compr[COMPR_NAMES.get(compr, hex(compr))] += 1
                if crc32(blob) != data_crc:
                    stats.bad_data_crc += 1
                payload = b""
                if dsize:
                    try:
                        payload = decompress(compr, blob, dsize)
                    except Exception as exc:  # noqa: BLE001
                        stats.dec_fail[f"{COMPR_NAMES.get(compr, hex(compr))}: {exc}"] += 1
                yield ("inode", version, ino, mode, uid, gid, isize, foffset, payload)
        else:
            yield ("other", nodetype, totlen)

        off += (totlen + 3) & ~3  # 节点按 4 字节对齐


# ---------------------------------------------------------------- rebuild tree


class Entry:
    __slots__ = ("ino", "mode", "uid", "gid", "isize", "pages", "path")

    def __init__(self, ino: int, mode: int, uid: int, gid: int) -> None:
        self.ino = ino
        self.mode = mode
        self.uid = uid
        self.gid = gid
        self.isize = 0
        self.pages: dict[int, tuple[int, bytes]] = {}
        self.path: str | None = None

    def data(self) -> bytes:
        if not self.pages:
            return b""
        end = max(o + len(p) for o, (_, p) in self.pages.items())
        buf = bytearray(end)
        for off, (_, payload) in sorted(self.pages.items()):
            buf[off:off + len(payload)] = payload
        return bytes(buf[: self.isize or len(buf)])


def extract(data: bytes, outdir: str | None, do_list: bool) -> int:
    stats = Stats()
    inodes: dict[int, Entry] = {}
    dirents: dict[tuple[int, str], tuple[int, int, int]] = {}  # (pino,name) -> (ver, ino, dtype)
    maxdtype: dict[int, int] = {}

    for ev in walk(data, stats):
        if ev[0] == "dirent":
            _, pino, version, ino, dtype, name = ev
            key = (pino, name)
            cur = dirents.get(key)
            if cur is None or version > cur[0]:
                dirents[key] = (version, ino, dtype)
            maxdtype[ino] = dtype
        elif ev[0] == "inode":
            _, version, ino, mode, uid, gid, isize, offset, payload = ev
            e = inodes.get(ino)
            if e is None:
                e = inodes[ino] = Entry(ino, mode, uid, gid)
            e.mode = mode
            e.uid, e.gid = uid, gid
            e.isize = max(e.isize, isize)
            prev = e.pages.get(offset)
            if prev is None or version > prev[0]:
                e.pages[offset] = (version, payload)

    # 自顶向下拼路径：根 ino=1
    children: dict[int, list[tuple[str, int]]] = defaultdict(list)
    for (pino, name), (_, ino, dtype) in dirents.items():
        children[pino].append((name, ino))

    root = inodes.get(ROOT_INO)
    if root is not None:
        root.path = ""
    stack = [(ROOT_INO, "")]
    seen = set()
    while stack:
        pino, prefix = stack.pop()
        for name, ino in children.get(pino, []):
            path = f"{prefix}/{name}"
            e = inodes.get(ino)
            if e is not None and e.path is None:
                e.path = path
            if ino in seen:
                continue
            seen.add(ino)
            stack.append((ino, path))

    # /.. 之类不可能是目录项，忽略；但 dirent 里可能有指向不存在 inode 的残留
    orphans = [i for i, e in inodes.items() if e.path is None and i != ROOT_INO]

    if do_list:
        rows = sorted(
            ((e.path or f"<ino {e.ino}>"), e) for e in inodes.values() if e.path is not None
        )
        for path, e in rows:
            kind = DT.get(maxdtype.get(e.ino, 8), "?")
            extra = ""
            if kind == "lnk":
                try:
                    extra = "  -> " + e.data().decode("utf-8", "replace")
                except Exception:  # noqa: BLE001
                    extra = "  -> ?"
            print(f"{kind:4} {e.isize:9d} {path}{extra}")

    print(
        "\n== nodes ==\n"
        + "\n".join(f"  {k:12} {v}" for k, v in sorted(stats.nodes.items()))
        + f"\n  endian {stats.endian}  inode-layout {stats.layout} {stats.layout_votes}"
        + f"\n  skipped(non-node 4B steps) {stats.skipped}"
        + f"\n  bad hdr_crc {stats.bad_hdr_crc}  bad data_crc {stats.bad_data_crc}"
        + "\n== compression ==\n"
        + "\n".join(f"  {k:10} {v}" for k, v in sorted(stats.compr.items()))
        + f"\n== files ==\n  inodes {len(inodes)}  dirents {len(dirents)}  orphans(no path) {len(orphans)}"
    )
    if stats.dec_fail:
        print("== decompress failures ==")
        for k, v in stats.dec_fail.items():
            print(f"  {v:6d}  {k}")

    if not outdir:
        return 0

    os.makedirs(outdir, exist_ok=True)
    wrote = 0
    for path, e in sorted(((e.path, e) for e in inodes.values() if e.path is not None)):
        if not path:
            continue
        dest = os.path.join(outdir, path.lstrip("/"))
        fmt = e.mode & 0o170000
        try:
            if fmt == 0o040000:
                os.makedirs(dest, exist_ok=True)
            elif fmt == 0o120000:
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                target = e.data().decode("utf-8", "replace")
                if os.path.lexists(dest):
                    os.unlink(dest)
                os.symlink(target, dest)
                wrote += 1
            elif fmt in (0o020000, 0o060000, 0o010000):
                continue  # 设备节点/FIFO：不重建（需要 root，且对分析无用）
            else:
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as fh:
                    fh.write(e.data())
                os.chmod(dest, (e.mode & 0o777) | 0o600)
                wrote += 1
        except OSError as exc:
            print(f"  !! {path}: {exc}", file=sys.stderr)
    print(f"\nwrote {wrote} entries into {outdir}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="JFFS2 extractor (stdlib only)")
    ap.add_argument("image", help="jffs2 image file")
    ap.add_argument("-o", "--outdir", help="extract into this directory")
    ap.add_argument("--list", action="store_true", help="print the file listing")
    args = ap.parse_args()

    with open(args.image, "rb") as fh:
        data = fh.read()
    print(f"image {args.image}  {len(data)} bytes  ({len(data) / 1048576:.2f} MiB)")
    return extract(data, args.outdir, args.list)


if __name__ == "__main__":
    raise SystemExit(main())
