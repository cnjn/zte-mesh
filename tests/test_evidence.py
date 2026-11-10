import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "dev"))

from jffs2 import EvidenceError, crc32, extract_file
from prepare import checked, resolve_library
from vendor_elf import ELF, ELFError, audit


def common(kind, length):
    prefix = struct.pack("<HHI", 0x1985, kind, length)
    return prefix + struct.pack("<I", crc32(prefix))


def dirent(parent, inode, name, kind, version=1):
    name = name.encode()
    head = common(0xE001, 40 + len(name))
    fields = struct.pack("<IIIIBBH", parent, version, inode, 0, len(name), kind, 0)
    return head + fields + struct.pack("<II", crc32(head + fields), crc32(name)) + name


def inode(data, compression=0, offset=0, size=None, version=1):
    raw = zlib.compress(data) if compression == 6 else (b"" if compression == 1 else data)
    head = common(0xE002, 68 + len(raw))
    fields = struct.pack("<IIIHHIIIIIIIBBH", 3, version, 0o100600, 0, 0,
                         len(data) if size is None else size, 0, 0, 0,
                         offset, len(raw), len(data), compression, 0, 0)
    return head + fields + struct.pack("<II", crc32(raw), crc32(head + fields)) + raw


def image(*chunks):
    all_nodes = [dirent(1, 2, "kmoduletmp", 4), dirent(2, 3, "kmodule.img", 8), *chunks]
    return b"".join(n + b"\xff" * ((-len(n)) % 4) for n in all_nodes)


def elf_fixture(defined=False):
    strings = b"\0libc.so.6\0example\0"
    symbols = bytes(16) + struct.pack("<IIIBBH", strings.index(b"example"), 0, 0, 0x12, 0, 1 if defined else 0)
    dynamic = struct.pack("<IIII", 1, 1, 0, 0)
    offset = 52 + 4 * 40
    sections = [bytes(40)]
    for kind, data, link, entry in [(3, strings, 0, 0), (11, symbols, 1, 16), (6, dynamic, 1, 8)]:
        sections.append(struct.pack("<IIIIIIIIII", 0, kind, 0, 0, offset, len(data), link, 0, 1, entry))
        offset += len(data)
    header = b"\x7fELF\x01\x01\x01" + bytes(9)
    header += struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, 0, 0, 52, 0x05000200, 52, 0, 0, 40, 4, 0)
    return header + b"".join(sections) + strings + symbols + dynamic


class JFFS2Tests(unittest.TestCase):
    def test_crc_linux_convention(self):
        self.assertEqual(crc32(b"123456789"), 0x2DFD2D88)

    def test_plain_zlib_zero(self):
        for compression, content in [(0, b"data"), (6, b"compressed" * 100), (1, bytes(20))]:
            self.assertEqual(extract_file(image(inode(content, compression)), "kmoduletmp/kmodule.img"), content)

    def test_latest_inode_wins(self):
        self.assertEqual(extract_file(image(inode(b"old", version=1), inode(b"new", version=2)), "kmoduletmp/kmodule.img"), b"new")

    def test_reject_data_crc_corruption(self):
        chunk = bytearray(inode(b"abc"))
        chunk[-1] ^= 1
        with self.assertRaisesRegex(EvidenceError, "data CRC"):
            extract_file(image(bytes(chunk)), "kmoduletmp/kmodule.img")

    def test_reject_header_crc(self):
        blob = bytearray(image(inode(b"abc")))
        blob[8] ^= 1
        with self.assertRaisesRegex(EvidenceError, "header CRC"):
            extract_file(bytes(blob), "kmoduletmp/kmodule.img")

    def test_reject_hole(self):
        with self.assertRaisesRegex(EvidenceError, "hole"):
            extract_file(image(inode(b"abc", offset=1, size=4)), "kmoduletmp/kmodule.img")

    def test_reject_duplicate_versions(self):
        with self.assertRaisesRegex(EvidenceError, "duplicate"):
            extract_file(image(inode(b"a"), inode(b"b")), "kmoduletmp/kmodule.img")

    def test_reject_large_file(self):
        with self.assertRaisesRegex(EvidenceError, "size limit"):
            extract_file(image(inode(b"a", size=100)), "kmoduletmp/kmodule.img", max_size=10)

    def test_reject_unsupported_compression(self):
        with self.assertRaisesRegex(EvidenceError, "compression"):
            extract_file(image(inode(b"abc", compression=7)), "kmoduletmp/kmodule.img")

    def test_path_validation(self):
        for path in ["/kmodule.img", "../kmodule.img", "kmoduletmp//kmodule.img"]:
            with self.assertRaises(EvidenceError):
                extract_file(b"", path)


class ELFTests(unittest.TestCase):
    def test_dependency_and_symbol_audit(self):
        consumer = ELF(elf_fixture())
        provider = ELF(elf_fixture(defined=True))
        consumer.require_vendor_abi()
        self.assertEqual(consumer.needed, ["libc.so.6"])
        self.assertEqual(consumer.imports, {"example"})
        self.assertEqual(audit({"consumer": consumer})["missing_strong_symbols"], ["example"])
        self.assertEqual(audit({"consumer": consumer, "libc.so.6": provider})["missing_strong_symbols"], [])

    def test_reject_non_vendor_abi(self):
        for location, fmt, value in [(18, "<H", 183), (36, "<I", 0x05000400)]:
            data = bytearray(elf_fixture())
            struct.pack_into(fmt, data, location, value)
            with self.assertRaises(ELFError):
                ELF(bytes(data)).require_vendor_abi()

    def test_reject_truncation(self):
        for data in [b"", elf_fixture()[:40], elf_fixture()[:-4]]:
            with self.assertRaises(ELFError):
                ELF(data)

    def test_reject_bad_string_offset(self):
        data = bytearray(elf_fixture())
        dynamic_offset = len(data) - 16
        struct.pack_into("<I", data, dynamic_offset + 4, 9999)
        with self.assertRaisesRegex(ELFError, "string offset"):
            ELF(bytes(data))

    def test_missing_version(self):
        consumer, provider = ELF(elf_fixture()), ELF(elf_fixture(True))
        consumer.version_needs = {"libc.so.6": {"GLIBC_2.4"}}
        self.assertTrue(audit({"main": consumer, "libc.so.6": provider})["missing_versions"])
        provider.version_defs = {"GLIBC_2.4"}
        self.assertFalse(audit({"main": consumer, "libc.so.6": provider})["missing_versions"])


class PackagingTests(unittest.TestCase):
    def test_refuse_hash_mismatch(self):
        with self.assertRaisesRegex(EvidenceError, "SHA256"):
            checked(b"unexpected", "0" * 64, "source")

    def test_resolve_alias_and_reject_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lib = root / "lib"
            lib.mkdir()
            (lib / "actual.so").write_bytes(b"ok")
            try:
                (lib / "alias.so").symlink_to("actual.so")
            except OSError as exc:
                if getattr(exc, "winerror", None) != 1314:
                    raise
                self.skipTest(f"symlinks unavailable on this host: {exc}")
            self.assertEqual(resolve_library(lib, "alias.so"), lib / "actual.so")
            (root / "outside.so").write_bytes(b"no")
            (lib / "escape.so").symlink_to("../outside.so")
            with self.assertRaises(EvidenceError):
                resolve_library(lib, "escape.so")

    def test_resolve_file_and_reject_invalid_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lib = root / "lib"
            lib.mkdir()
            (lib / "actual.so").write_bytes(b"ok")
            (root / "outside.so").write_bytes(b"no")
            self.assertEqual(resolve_library(lib, "actual.so"), lib / "actual.so")
            for name in ["../outside.so", "missing.so"]:
                with self.subTest(name=name), self.assertRaises(EvidenceError):
                    resolve_library(lib, name)


if __name__ == "__main__":
    unittest.main()
