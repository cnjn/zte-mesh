"""Read a single regular file from a little-endian JFFS2 evidence image.

This is deliberately not a general filesystem extractor. Only uncompressed,
zero-filled and zlib inode data are supported; bad CRCs and holes are errors.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass


class EvidenceError(ValueError):
    pass


def crc32(data: bytes) -> int:
    # JFFS2 uses Linux crc32(seed=0), not zlib's default initial/final XOR.
    return (zlib.crc32(data, 0xFFFFFFFF) ^ 0xFFFFFFFF) & 0xFFFFFFFF


@dataclass(frozen=True)
class Node:
    offset: int
    kind: int
    data: bytes


def nodes(image: bytes):
    offset = 0
    while offset + 12 <= len(image):
        magic, kind, length, checksum = struct.unpack_from("<HHII", image, offset)
        if magic != 0x1985:
            offset += 4
            continue
        if length < 12 or offset + length > len(image):
            raise EvidenceError(f"invalid JFFS2 node length at {offset:#x}")
        if crc32(image[offset:offset + 8]) != checksum:
            raise EvidenceError(f"JFFS2 header CRC mismatch at {offset:#x}")
        yield Node(offset, kind, image[offset:offset + length])
        offset += (length + 3) & ~3


def extract_file(image: bytes, path: str, max_size: int = 32 * 1024 * 1024) -> bytes:
    parts = path.split("/")
    if not parts or any(p in ("", ".", "..") for p in parts):
        raise EvidenceError("expected a relative, canonical JFFS2 path")
    entries = {}
    inodes = {}
    for node in nodes(image):
        data = node.data
        if node.kind == 0xE001:
            if len(data) < 40:
                raise EvidenceError("short JFFS2 dirent")
            parent, version, inode = struct.unpack_from("<III", data, 12)
            name_size, file_type = data[28:30]
            if len(data) != 40 + name_size:
                raise EvidenceError("JFFS2 dirent name length mismatch")
            name = data[40:]
            node_crc, name_crc = struct.unpack_from("<II", data, 32)
            if crc32(data[:32]) != node_crc or crc32(name) != name_crc:
                raise EvidenceError(f"JFFS2 dirent CRC mismatch at {node.offset:#x}")
            key = (parent, name)
            previous = entries.get(key)
            if previous is None or version > previous[0]:
                entries[key] = (version, inode, file_type)
        elif node.kind == 0xE002:
            if len(data) < 68:
                raise EvidenceError("short JFFS2 inode")
            inode = struct.unpack_from("<I", data, 12)[0]
            inodes.setdefault(inode, []).append(node)

    inode = 1
    for index, part in enumerate(parts):
        entry = entries.get((inode, part.encode("utf-8")))
        if not entry or not entry[1]:
            raise EvidenceError(f"JFFS2 path not found: {path}")
        inode = entry[1]
        expected_type = 8 if index == len(parts) - 1 else 4
        if entry[2] != expected_type:
            raise EvidenceError("JFFS2 path is not directory/regular file")

    chunks = sorted(inodes.get(inode, []), key=lambda n: struct.unpack_from("<I", n.data, 16)[0])
    if not chunks:
        raise EvidenceError("JFFS2 file has no inode data")
    size = struct.unpack_from("<I", chunks[-1].data, 28)[0]
    if size > max_size:
        raise EvidenceError("JFFS2 file exceeds size limit")
    result = bytearray(size)
    ranges = []
    versions = set()
    for node in chunks:
        data = node.data
        version, mode = struct.unpack_from("<II", data, 16)
        if version in versions:
            raise EvidenceError("duplicate JFFS2 inode version")
        versions.add(version)
        if mode & 0o170000 != 0o100000:
            raise EvidenceError("JFFS2 inode is not a regular file")
        offset, compressed_size, plain_size = struct.unpack_from("<III", data, 44)
        compression = data[56]
        data_crc, node_crc = struct.unpack_from("<II", data, 60)
        if crc32(data[:60]) != node_crc:
            raise EvidenceError(f"JFFS2 inode CRC mismatch at {node.offset:#x}")
        if compressed_size != len(data) - 68:
            raise EvidenceError("JFFS2 inode data length mismatch")
        raw = data[68:]
        if crc32(raw) != data_crc:
            raise EvidenceError(f"JFFS2 data CRC mismatch at {node.offset:#x}")
        if plain_size > max_size:
            raise EvidenceError("JFFS2 chunk exceeds size limit")
        if compression == 0:
            plain = raw
        elif compression == 1:
            plain = bytes(plain_size)
        elif compression == 6:
            try:
                decoder = zlib.decompressobj()
                plain = decoder.decompress(raw, plain_size + 1)
                if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                    raise EvidenceError("incomplete/oversized JFFS2 zlib stream")
            except zlib.error as error:
                raise EvidenceError("invalid JFFS2 zlib data") from error
        else:
            raise EvidenceError(f"unsupported JFFS2 compression: {compression}")
        if len(plain) != plain_size:
            raise EvidenceError("JFFS2 decompressed size mismatch")
        end = min(size, offset + plain_size)
        if offset < end:
            result[offset:end] = plain[:end - offset]
            ranges.append((offset, end))
    covered = 0
    for start, end in sorted(ranges):
        if start > covered:
            raise EvidenceError(f"JFFS2 file has a hole at {covered:#x}")
        covered = max(covered, end)
    if covered != size:
        raise EvidenceError("JFFS2 file has a trailing hole")
    return bytes(result)
