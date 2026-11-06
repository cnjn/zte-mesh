#!/usr/bin/env python3
"""unpack_squashfs.py -- 纯标准库的 squashfs 4.0 解开器（没有 unsquashfs 时用）

用法:
    python tools/lab/unpack_squashfs.py kmodule.img --list
    python tools/lab/unpack_squashfs.py kmodule.img -o out/

支持压缩: gzip / lzma / xz；lz4 / zstd 需要同名 python 模块（`lz4.block` / `zstandard`），
缺模块时只对用到它的文件报错，不中断其它文件。

为什么自己写: 这台机器上没有 unsquashfs（Windows），pip 也装不了东西。E2631 的
`/kmodule` 就是 rootfs 里的 `/kmoduletmp/kmodule.img`（13.2 MB 小端 squashfs 4.0），
`/kmodule/usr` 里才是 Web 页面与 CGI。

关键结构（全部小端）:
  superblock 96 B      inode_table_start / directory_table_start / fragment_table_start
  metadata block       u16 头（bit15=未压缩，低 15 位=长度）+ 数据
  dir listing          u32 count/start/inode 头 12 B，然后每项 8 B + 名字
  fragment 表          先是若干 u64 指针，指向装着 16 B 条目的 metadata block
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

MAGIC = 0x73717368  # "hsqs"

COMP = {0: "none", 1: "gzip", 2: "lzma", 3: "lzo", 4: "xz", 5: "lz4", 6: "zstd"}

I_DIR, I_FILE, I_SYMLINK, I_BLKDEV, I_CHRDEV, I_FIFO, I_SOCKET = range(1, 8)
I_LDIR, I_LFILE, I_LSYMLINK, I_LBLKDEV, I_LCHRDEV, I_LFIFO, I_LSOCKET = range(8, 15)

NO_FRAGMENT = 0xFFFFFFFF
FLAG_UNCOMPRESSED_DATA = 0x0002


def _zlib(b: bytes, n: int) -> bytes:
    import zlib

    return zlib.decompress(b)[:n]


def _xz(b: bytes, n: int) -> bytes:
    import lzma

    return lzma.decompress(b, format=lzma.FORMAT_XZ)[:n]


def _lzma_alone(b: bytes, n: int) -> bytes:
    import lzma

    return lzma.decompress(b, format=lzma.FORMAT_ALONE)[:n]


def _lz4(b: bytes, n: int) -> bytes:
    import lz4.block  # type: ignore

    return lz4.block.decompress(b, uncompressed_size=n)[:n]


def _zstd(b: bytes, n: int) -> bytes:
    import zstandard  # type: ignore

    return zstandard.ZstdDecompressor().decompress(b, max_output_size=n)[:n]


DECOMP = {1: _zlib, 2: _lzma_alone, 4: _xz, 5: _lz4, 6: _zstd}


class Cursor:
    """在某个 metadata 表里顺序读字节，自动跨 block。"""

    def __init__(self, fs: "Squash", table_base: int, block: int, offset: int) -> None:
        self.fs = fs
        self.base = table_base
        self.pos = table_base + block      # 当前 metadata block 的磁盘位置
        self.blob, self.next = fs.meta_block(self.pos)
        self.off = offset

    def read(self, n: int) -> bytes:
        out = b""
        while n > 0:
            if self.off >= len(self.blob):
                self.pos = self.next
                self.blob, self.next = self.fs.meta_block(self.pos)
                self.off = 0
                continue
            take = min(n, len(self.blob) - self.off)
            out += self.blob[self.off:self.off + take]
            self.off += take
            n -= take
        return out

    def here(self) -> tuple[int, int]:
        return self.pos - self.base, self.off


class Squash:
    def __init__(self, path: str) -> None:
        with open(path, "rb") as fh:
            self.data = fh.read()
        sb = struct.unpack_from("<IIIIIHHHHHHQQQQQQQQ", self.data, 0)
        (magic, self.ninodes, self.mkfs_time, self.block_size, self.frag_count,
         self.comp_id, self.block_log, self.flags, self.no_ids, self.s_major,
         self.s_minor, self.root_inode, self.bytes_used, self.id_table_start,
         self.xattr_id_table_start, self.inode_table_start,
         self.directory_table_start, self.fragment_table_start,
         self.lookup_table_start) = sb
        if magic != MAGIC:
            raise SystemExit("not a squashfs image (magic != hsqs)")
        if (self.s_major, self.s_minor) != (4, 0):
            raise SystemExit(f"only squashfs 4.0 supported, got {self.s_major}.{self.s_minor}")
        if self.comp_id not in DECOMP and self.comp_id != 0:
            raise SystemExit(f"unsupported compression {COMP.get(self.comp_id, self.comp_id)}")
        self._meta: dict[int, tuple[bytes, int]] = {}

    # -------------------------------------------------------------- metadata

    def _inflate(self, blob: bytes, size: int) -> bytes:
        if self.comp_id == 0:
            return blob[:size]
        return DECOMP[self.comp_id](blob, size)

    def meta_block(self, pos: int) -> tuple[bytes, int]:
        hit = self._meta.get(pos)
        if hit is not None:
            return hit
        hdr = struct.unpack_from("<H", self.data, pos)[0]
        size = hdr & 0x7FFF
        raw = self.data[pos + 2:pos + 2 + size]
        blob = raw if hdr & 0x8000 else self._inflate(raw, 8192)
        out = (blob, pos + 2 + size)
        self._meta[pos] = out
        return out

    def read_at(self, table_base: int, block: int, offset: int, n: int) -> bytes:
        return Cursor(self, table_base, block, offset).read(n)

    # ---------------------------------------------------------------- inodes

    def inode(self, ref: int) -> dict:
        block, offset = ref >> 16, ref & 0xFFFF
        cur = Cursor(self, self.inode_table_start, block, offset)
        itype, mode, uid, gid, mtime, ino = struct.unpack("<HHHHII", cur.read(16))
        node = {"type": itype, "mode": mode, "uid": uid, "gid": gid, "mtime": mtime, "ino": ino}
        ref_of = (block, offset)

        if itype == I_DIR:
            start, nlink, fsize, doff, parent = struct.unpack("<IIHHI", cur.read(16))
            node.update(kind="dir", dir_block=start, dir_offset=doff, file_size=fsize, parent=parent)
        elif itype == I_LDIR:
            nlink, fsize, start, parent, icount, doff, _xattr = struct.unpack("<IIIIHHI", cur.read(24))
            node.update(kind="dir", dir_block=start, dir_offset=doff, file_size=fsize, parent=parent)
        elif itype == I_FILE:
            start, frag, foff, fsize = struct.unpack("<IIII", cur.read(16))
            node.update(kind="file", start_block=start, fragment=frag, frag_off=foff,
                        file_size=fsize, hdr=32)
        elif itype == I_LFILE:
            start, fsize, sparse, nlink, frag, foff, _xattr = struct.unpack("<QQQIIII", cur.read(40))
            node.update(kind="file", start_block=start, fragment=frag, frag_off=foff,
                        file_size=fsize, hdr=56)
        elif itype in (I_SYMLINK, I_LSYMLINK):
            nlink, ssize = struct.unpack("<II", cur.read(8))
            node.update(kind="symlink", nlink=nlink, target=cur.read(ssize))
        elif itype in (I_BLKDEV, I_CHRDEV, I_LBLKDEV, I_LCHRDEV):
            nlink, rdev = struct.unpack("<II", cur.read(8))
            node.update(kind="dev", nlink=nlink, rdev=rdev)
        else:
            node.update(kind="ipc")
        node["_ref"] = ref_of
        return node

    def dir_entries(self, node: dict) -> list[tuple[str, int]]:
        """返回 [(name, inode_ref)]，按写入顺序。"""
        cur = Cursor(self, self.directory_table_start, node["dir_block"], node["dir_offset"])
        remaining = node["file_size"] - 3
        out: list[tuple[str, int]] = []
        while remaining > 0:
            count, start, _ino = struct.unpack("<III", cur.read(12))
            remaining -= 12
            for _ in range(count + 1):
                offset, _delta, _etype, nsize = struct.unpack("<HhHH", cur.read(8))
                name = cur.read(nsize + 1)
                remaining -= 8 + nsize + 1
                out.append((name.decode("utf-8", "replace"), (start << 16) | offset))
            if remaining < 0:
                break
        return out

    # ------------------------------------------------------------------ data

    def raw_block(self, start: int, size: int) -> bytes:
        if size == 0:
            return b""
        raw = self.data[start:start + size]
        if self.flags & FLAG_UNCOMPRESSED_DATA:
            return raw
        try:
            return self._inflate(raw, self.block_size)
        except Exception:  # noqa: BLE001  —— 未压缩块会解压失败，按原样返回
            return raw

    def fragment(self, index: int) -> bytes:
        ptr = struct.unpack_from("<Q", self.data, self.fragment_table_start + index // 512 * 8)[0]
        blob, _ = self.meta_block(ptr)
        start, size, _unused = struct.unpack_from("<QII", blob, (index % 512) * 16)
        return self.raw_block(start, size)

    def read_file(self, node: dict) -> bytes:
        size = node["file_size"]
        bs = self.block_size
        block, offset = node["_ref"]
        cur = Cursor(self, self.inode_table_start, block, offset + node["hdr"])
        nblocks = size // bs if node["fragment"] != NO_FRAGMENT else (size + bs - 1) // bs
        sizes = struct.unpack(f"<{nblocks}I", cur.read(nblocks * 4)) if nblocks else ()
        out = bytearray()
        pos = node["start_block"]
        for s in sizes:
            if s == 0:
                out += b"\x00" * bs
                continue
            out += self.raw_block(pos, s)
            pos += s
        if node["fragment"] != NO_FRAGMENT:
            blob = self.fragment(node["fragment"])
            tail = size - len(out)
            if tail > 0:
                out += blob[node["frag_off"]:node["frag_off"] + tail]
        return bytes(out[:size])


def walk(fs: Squash, outdir: str | None, do_list: bool) -> int:
    stats = {"dir": 0, "file": 0, "symlink": 0, "dev": 0, "ipc": 0, "written": 0, "err": 0}
    stack: list[tuple[str, int]] = [("", fs.root_inode)]
    while stack:
        path, ref = stack.pop()
        node = fs.inode(ref)
        kind = node["kind"]
        stats[kind] = stats.get(kind, 0) + 1

        if kind == "dir":
            if do_list:
                print(f"dir          0 {path or '/'}")
            if outdir and path:
                os.makedirs(os.path.join(outdir, path.lstrip("/")), exist_ok=True)
            for name, child in fs.dir_entries(node):
                if name in (".", ".."):
                    continue
                stack.append((f"{path}/{name}", child))
            continue

        if do_list:
            extra = ""
            if kind == "symlink":
                extra = "  -> " + node["target"].decode("utf-8", "replace")
            print(f"{kind:6} {node.get('file_size', 0):9d} {path}{extra}")

        if not outdir or kind not in ("file", "symlink"):
            continue
        dest = os.path.join(outdir, path.lstrip("/"))
        try:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if kind == "file":
                with open(dest, "wb") as fh:
                    fh.write(fs.read_file(node))
            else:
                if os.path.lexists(dest):
                    os.unlink(dest)
                os.symlink(node["target"].decode("utf-8", "replace"), dest)
            stats["written"] += 1
        except OSError as exc:
            stats["err"] += 1
            print(f"  !! {path}: {exc}", file=sys.stderr)

    print("\n" + "  ".join(f"{k} {v}" for k, v in stats.items()))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="squashfs 4.0 extractor (stdlib only)")
    ap.add_argument("image")
    ap.add_argument("-o", "--outdir")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    fs = Squash(args.image)
    print(f"{args.image}: squashfs {fs.s_major}.{fs.s_minor}  inodes={fs.ninodes} "
          f"block={fs.block_size} frags={fs.frag_count} "
          f"comp={COMP.get(fs.comp_id, fs.comp_id)} flags={fs.flags:#06x} "
          f"bytes_used={fs.bytes_used}")
    return walk(fs, args.outdir, args.list)


if __name__ == "__main__":
    raise SystemExit(main())
