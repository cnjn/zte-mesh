"""Bounded ELF metadata reader for the vendor ARM32 runtime (no execution)."""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path


class ELFError(ValueError):
    pass


@dataclass
class ELF:
    data: bytes = field(repr=False)
    machine: int = field(init=False)
    flags: int = field(init=False)
    elf_type: int = field(init=False)
    interpreter: str | None = field(init=False, default=None)
    needed: list[str] = field(init=False, default_factory=list)
    imports: set[str] = field(init=False, default_factory=set)
    exports: set[str] = field(init=False, default_factory=set)
    functions: dict[str, dict] = field(init=False, default_factory=dict)
    version_needs: dict[str, set[str]] = field(init=False, default_factory=dict)
    version_defs: set[str] = field(init=False, default_factory=set)

    @classmethod
    def read(cls, path: Path) -> "ELF":
        return cls(path.read_bytes())

    def unpack(self, fmt: str, offset: int):
        size = struct.calcsize(fmt)
        self.region(offset, size)
        return struct.unpack_from(fmt, self.data, offset)

    def region(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ELFError("ELF region outside file")
        return self.data[offset:offset + size]

    @staticmethod
    def string(table: bytes, offset: int) -> str:
        if offset >= len(table):
            raise ELFError("ELF string offset outside table")
        end = table.find(b"\0", offset)
        if end < 0:
            raise ELFError("unterminated ELF string")
        try:
            return table[offset:end].decode("utf-8")
        except UnicodeDecodeError as error:
            raise ELFError("invalid ELF string") from error

    def __post_init__(self):
        if len(self.data) < 52 or self.data[:7] != b"\x7fELF\x01\x01\x01":
            raise ELFError("expected little-endian ELF32")
        header = self.unpack("<HHIIIIIHHHHHH", 16)
        self.elf_type, self.machine = header[:2]
        phoff, shoff, self.flags = header[4:7]
        ehsize, phsize, phnum, shsize, shnum, _ = header[7:]
        if ehsize != 52 or (phnum and phsize != 32) or shsize != 40 or not shnum:
            raise ELFError("unsupported ELF header/table dimensions")
        self.region(phoff, phsize * phnum)
        self.region(shoff, shsize * shnum)
        sections = [self.unpack("<IIIIIIIIII", shoff + i * shsize) for i in range(shnum)]
        for i in range(phnum):
            ph = self.unpack("<IIIIIIII", phoff + i * phsize)
            if ph[0] == 3:
                value = self.region(ph[1], ph[4])
                if not value.endswith(b"\0"):
                    raise ELFError("unterminated ELF interpreter")
                self.interpreter = value[:-1].decode("ascii")
        for section in sections:
            kind, offset, size, link, entry_size = section[1], section[4], section[5], section[6], section[9]
            if kind not in (2, 6, 11, 0x6FFFFFFD, 0x6FFFFFFE):
                continue
            payload = self.region(offset, size)
            if link >= len(sections):
                raise ELFError("ELF linked section outside table")
            strings = sections[link]
            table = self.region(strings[4], strings[5])
            if kind == 6:
                if entry_size != 8 or size % 8:
                    raise ELFError("invalid ELF dynamic table")
                for pos in range(0, size, 8):
                    tag, value = struct.unpack_from("<II", payload, pos)
                    if tag == 1:
                        name = self.string(table, value)
                        if not name or "/" in name or name in (".", ".."):
                            raise ELFError("unsafe ELF dependency name")
                        self.needed.append(name)
            elif kind in (2, 11):
                if entry_size != 16 or size % 16:
                    raise ELFError("invalid ELF symbol table")
                for pos in range(0, size, 16):
                    name, value, length, info, other, index = struct.unpack_from("<IIIBBH", payload, pos)
                    if not name:
                        continue
                    text = self.string(table, name)
                    if kind == 2 and index and info & 15 == 2:
                        self.functions[text] = {"address": hex(value), "size": length}
                    if kind != 11 or info >> 4 not in (1, 2):
                        continue
                    if index and other & 3 in (0, 3):
                        self.exports.add(text)
                    elif not index and info >> 4 == 1:
                        self.imports.add(text)
            else:
                self._versions(payload, table, kind)

    def _versions(self, payload: bytes, table: bytes, kind: int):
        pos = 0
        visited = set()
        while pos < len(payload):
            if pos in visited:
                raise ELFError("cyclic ELF version table")
            visited.add(pos)
            try:
                if kind == 0x6FFFFFFE:
                    version, count, file_name, auxiliary, following = struct.unpack_from("<HHIII", payload, pos)
                    provider = self.string(table, file_name)
                    names = self.version_needs.setdefault(provider, set())
                    aux = pos + auxiliary
                    aux_seen = set()
                    for index in range(count):
                        if aux in aux_seen:
                            raise ELFError("cyclic ELF version auxiliary table")
                        aux_seen.add(aux)
                        _, flags, _, name, next_aux = struct.unpack_from("<IHHII", payload, aux)
                        if not flags & 2:  # VER_FLG_WEAK is optional.
                            names.add(self.string(table, name))
                        if index + 1 < count and not next_aux:
                            raise ELFError("short ELF version auxiliary table")
                        aux += next_aux
                else:
                    version, _, _, count, _, auxiliary, following = struct.unpack_from("<HHHHIII", payload, pos)
                    if not count:
                        raise ELFError("empty ELF version definition")
                    name, _ = struct.unpack_from("<II", payload, pos + auxiliary)
                    self.version_defs.add(self.string(table, name))
                if version != 1:
                    raise ELFError("unsupported ELF version table")
            except struct.error as error:
                raise ELFError("truncated ELF version table") from error
            if not following:
                break
            pos += following
            if pos >= len(payload):
                raise ELFError("ELF version next offset outside section")

    def require_vendor_abi(self):
        if self.machine != 40 or self.flags != 0x05000200 or self.elf_type not in (2, 3):
            raise ELFError(f"not vendor ARM EABI5 soft-float ABI: machine={self.machine}, flags={self.flags:#x}")

    def summary(self) -> dict:
        return {
            "class": 32, "endianness": "little", "machine": self.machine,
            "flags": hex(self.flags), "type": self.elf_type,
            "interpreter": self.interpreter, "needed": self.needed,
            "strong_imports": len(self.imports), "exports": len(self.exports),
            "version_needs": {k: sorted(v) for k, v in sorted(self.version_needs.items())},
        }


def audit(objects: dict[str, ELF]) -> dict:
    provided = set().union(*(obj.exports for obj in objects.values()))
    required = set().union(*(obj.imports for obj in objects.values()))
    missing_versions = []
    for name, obj in objects.items():
        obj.require_vendor_abi()
        for provider, versions in obj.version_needs.items():
            available = objects.get(provider)
            missing = versions - (available.version_defs if available else set())
            if missing:
                missing_versions.append({"object": name, "provider": provider, "versions": sorted(missing)})
    return {"missing_strong_symbols": sorted(required - provided), "missing_versions": missing_versions}
