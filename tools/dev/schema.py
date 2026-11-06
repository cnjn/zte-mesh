#!/usr/bin/env python3
"""Public MultiAPCfg registry and hash-pinned, data-only constructor verification.

The tiny ARM decoder below understands ONLY the straight-line initializer in
this one baseline. It never executes cspd, follows another function, loads a
DSO, performs a syscall, or accepts an unknown instruction/call target.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

from vendor_elf import ELF

CSPD_SHA256 = "8b8477e2598660653c27d796fcaaa5484f879cd263fb6caa5e0a982b1cee5ac1"
DB_SHA256 = "b8316a82f3726eddcf96dfa997c7a55bd08f825dc9808167600f751c302f7a1f"
TYPE_LENGTHS = {2: (36, 64), 3: (68, 96), 5: (260, 288), 7: (1028, 1056),
                12: (4, 32), 13: (4, 32), 14: (4, 32), 15: (1, 32)}
PATTERN = re.compile(r'^MAP_FIELD\(([A-Z0-9_]+), "([^"]+)", (\d+), ([01]), "([^"]*)"\)$')


def registry(path=None):
    path = path or Path(__file__).with_name("multiap_cfg.def")
    fields, offset = [], 8
    for line in path.read_text().splitlines():
        if not line.startswith("MAP_FIELD"):
            continue
        match = PATTERN.fullmatch(line)
        if not match:
            raise ValueError("malformed registry entry")
        ident, name, dtype, saved, default = match.groups()
        dtype, saved = int(dtype), int(saved)
        if dtype not in TYPE_LENGTHS or len(name.encode()) > 32:
            raise ValueError("unsupported field type/name")
        value_length, view_length = TYPE_LENGTHS[dtype]
        if dtype in (2, 3, 5, 7):
            if len(default.encode("ascii")) >= value_length - 4:
                raise ValueError("registration string default too long")
        else:
            if not re.fullmatch(r"-?[0-9]+", default):
                raise ValueError("non-numeric registration default")
            low, high = (-2147483648, 2147483647) if dtype == 12 else (0, 255 if dtype == 15 else 4294967295)
            if not low <= int(default) <= high:
                raise ValueError("registration default outside native type range")
        fields.append({"id": ident, "name": name, "type": dtype, "save": saved,
                       "default": default, "record_offset": offset,
                       "value_offset": offset + 28, "value_length": value_length,
                       "record_length": view_length})
        offset += view_length
    if (len(fields) != 50 or offset != 2952 or
            len({f["name"] for f in fields}) != 50 or len({f["id"] for f in fields}) != 50):
        raise ValueError("incorrect MultiAPCfg field count/size/uniqueness")
    return {"view": "MultiAPCfg", "size": offset, "magic": "0x01020304",
            "source_sha256": CSPD_SHA256, "fields": fields,
            "controller_ready": False}


class Image:
    def __init__(self, path, expected_hash):
        self.elf = ELF.read(path)
        self.elf.require_vendor_abi()
        if hashlib.sha256(self.elf.data).hexdigest() != expected_hash:
            raise ValueError("firmware baseline mismatch")
        header = self.elf.unpack("<HHIIIIIHHHHHH", 16)
        self.loads = [self.elf.unpack("<8I", header[4] + i * 32)
                      for i in range(header[9])]

    def read(self, address, size):
        for kind, offset, base, _, length, _, _, _ in self.loads:
            if kind == 1 and base <= address and address + size <= base + length:
                return self.elf.region(offset + address - base, size)
        raise ValueError("address outside file-backed ELF segment")

    def string(self, address):
        data = bytearray()
        for i in range(256):
            value = self.read(address + i, 1)[0]
            if not value:
                return data.decode("ascii")
            data.append(value)
        raise ValueError("unbounded initializer string")


CONSTRUCTORS = {
    "MultiAPCfg": (0x1f16c0, 0x1f1b64, 3468),
    "MultiAPMaster": (0x1f2218, 0x1f26fc, 3808),
    "MultiAPDomain": (0x1f2900, 0x1f3264, 7276),
}


def constructor_data(path, table_name="MultiAPCfg"):
    """Only decode the three hash-pinned initializers, never runtime defaults."""
    if table_name not in CONSTRUCTORS:
        raise ValueError("unknown constructor")
    return _decode_constructor(path, CONSTRUCTORS[table_name])


def _decode_constructor(path, spec, analysis_calls=None):
    """Bounded data decoder; additional calls are fixed, non-executing models."""
    start, stop, domain_bytes = spec
    analysis_calls = analysis_calls or {}
    image = Image(path, CSPD_SHA256)
    registers = [0] * 16
    registers[13] = 0x80010000
    stack_base, stack = 0x8000e000, bytearray(8192)
    recovered = {}

    def read(address, size):
        if stack_base <= address and address + size <= stack_base + len(stack):
            return bytes(stack[address - stack_base:address - stack_base + size])
        return image.read(address, size)

    def write(address, data):
        if not stack_base <= address or address + len(data) > stack_base + len(stack):
            raise ValueError("initializer write outside private analysis stack")
        stack[address - stack_base:address - stack_base + len(data)] = data

    # Stop before the final pop; no emulated control transfer is needed.
    for pc in range(start, stop, 4):
        word = struct.unpack("<I", image.read(pc, 4))[0]
        if word >> 28 != 14:
            raise ValueError("conditional initializer instruction")
        registers[15] = pc + 8
        if word & 0xffff0000 == 0xe92d0000:  # push a fixed register list
            saved = [r for r in range(16) if word & (1 << r)]
            if 13 in saved or 15 in saved:
                raise ValueError("unsupported initializer push")
            registers[13] -= len(saved) * 4
            write(registers[13], struct.pack("<" + "I" * len(saved), *(registers[r] for r in saved)))
        elif word & 0x0ff00000 == 0x03000000:  # movw
            registers[(word >> 12) & 15] = ((word >> 4) & 0xf000) | (word & 0xfff)
        elif word & 0x0f000000 == 0x0b000000:  # bl: four exact permitted data operations
            delta = word & 0xffffff
            if delta & 0x800000:
                delta -= 1 << 24
            target = pc + 8 + delta * 4
            if target == 0x1e5fc:  # memset
                if registers[1] != 0 or registers[2] > 8192:
                    raise ValueError("unexpected initializer memset")
                write(registers[0], bytes(registers[2]))
            elif target == 0x204f8:  # capture descriptor, do NOT call dbCreateTbl
                recovered["table"] = read(registers[0], 164)
                registers[0] = 0x70000000
            elif target == 0x1e50c:  # capture descriptors, do NOT call dbCreateDomainNew
                if registers[0] != 0x70000000 or registers[2] != domain_bytes:
                    raise ValueError("unexpected domain descriptor operation")
                recovered["domains"] = read(registers[1], registers[2])
                registers[0] = 0
            elif pc in analysis_calls and analysis_calls[pc][0] == target:
                # The model may inspect metadata/private analysis bytes only.
                # Never invoke the original function or any other ELF code.
                analysis_calls[pc][1](registers, recovered, image)
            else:
                raise ValueError("unknown initializer call target")
        elif (word >> 26) & 3 == 1:  # immediate ldr/str, no writeback/shifted register
            if word & (1 << 25) or not word & (1 << 24) or word & (1 << 21):
                raise ValueError("unsupported initializer memory mode")
            rn, rd = (word >> 16) & 15, (word >> 12) & 15
            delta = word & 0xfff
            address = registers[rn] + (delta if word & (1 << 23) else -delta)
            size = 1 if word & (1 << 22) else 4
            if word & (1 << 20):
                registers[rd] = int.from_bytes(read(address, size), "little")
            else:
                write(address, (registers[rd] & ((1 << (8 * size)) - 1)).to_bytes(size, "little"))
        elif (word >> 26) & 3 == 0:  # mov/add/sub with literal or unshifted register
            opcode, rn, rd = (word >> 21) & 15, (word >> 16) & 15, (word >> 12) & 15
            if word & (1 << 20):
                raise ValueError("initializer changes flags")
            if word & (1 << 25):
                shift, literal = ((word >> 8) & 15) * 2, word & 255
                operand = ((literal >> shift) | (literal << ((32 - shift) % 32))) & 0xffffffff
            else:
                if word & 0xff0:
                    raise ValueError("shifted initializer operand")
                operand = registers[word & 15]
            if opcode == 13:
                value = operand
            elif opcode == 4:
                value = registers[rn] + operand
            elif opcode == 2:
                value = registers[rn] - operand
            else:
                raise ValueError("unknown initializer arithmetic")
            registers[rd] = value & 0xffffffff
        else:
            raise ValueError("unknown initializer instruction")
    if set(recovered) != {"table", "domains"}:
        raise ValueError("incomplete constructor")
    return image, recovered["table"], recovered["domains"]


def recover_constructor(path):
    image, table_data, domain_data = constructor_data(path)
    table = bytearray(164)
    struct.pack_into("<III", table, 0, struct.unpack_from("<I", table_data)[0], 0, 1)
    table[12] = 1
    if table_data != table or image.string(struct.unpack_from("<I", table)[0]) != "MultiAPCfg":
        raise ValueError("unexpected table descriptor")
    fields = []
    for index in range(51):
        data = domain_data[index * 68:(index + 1) * 68]
        name_ptr, _, dtype = struct.unpack_from("<III", data)
        default_ptr = struct.unpack_from("<I", data, 24)[0]
        if index == 50:
            if image.string(name_ptr) != "":
                raise ValueError("missing domain terminator")
            break
        expected = bytearray(68)
        struct.pack_into("<III", expected, 0, name_ptr, 0, dtype)
        expected[13] = data[13]
        struct.pack_into("<I", expected, 24, default_ptr)
        if expected != data or data[13] not in (0, 1):
            raise ValueError("unexpected domain bounds/flags/pointers")
        fields.append({"name": image.string(name_ptr), "type": dtype,
                       "save": data[13], "default": image.string(default_ptr)})
    return fields


def verify_constructor(path, schema):
    actual = recover_constructor(path)
    expected = [{k: f[k] for k in ("name", "type", "save", "default")} for f in schema["fields"]]
    if actual != expected:
        changed = [f["name"] for f, g in zip(expected, actual) if f != g]
        raise ValueError(f"registry differs from pinned constructor: {changed}")


POLICY_TABLES = {
    "MultiAPMaster": {"file": "multiap_master.def", "count": 55, "size": 2312, "max_rows": 1},
    "MultiAPDomain": {"file": "multiap_domain.def", "count": 106, "size": 3464, "max_rows": 32},
}
POLICY_PATTERN = re.compile(r'^MESH_FIELD\("([^"]+)", (\d+), ([01]), ([01]), "([^"]*)"\)$')


def policy_registry(table_name, path=None):
    if table_name not in POLICY_TABLES:
        raise ValueError("unknown policy table")
    spec = POLICY_TABLES[table_name]
    path = path or Path(__file__).with_name(spec["file"])
    fields, offset = [], 8
    for line in path.read_text().splitlines():
        if not line.startswith("MESH_FIELD"):
            continue
        match = POLICY_PATTERN.fullmatch(line)
        if not match:
            raise ValueError("malformed policy field")
        name, dtype, primary, saved, default = match.groups()
        dtype, primary, saved = int(dtype), int(primary), int(saved)
        if dtype not in TYPE_LENGTHS or len(name.encode("ascii")) > 32:
            raise ValueError("unsupported policy field type/name")
        value_length, record_length = TYPE_LENGTHS[dtype]
        if dtype in (2, 3, 5, 7):
            # These policy tables have no nonempty string registration defaults.
            # Explicit test ESSIDs/keys belong in the private fixture, not here.
            if default != "":
                raise ValueError("nonempty policy string default forbidden")
        else:
            if not re.fullmatch(r"-?[0-9]+", default):
                raise ValueError("non-numeric policy default")
            low, high = (-2147483648, 2147483647) if dtype == 12 else (0, 255 if dtype == 15 else 4294967295)
            if not low <= int(default) <= high:
                raise ValueError("policy default outside native type range")
        fields.append({"name": name, "type": dtype, "primary": primary, "save": saved,
                       "default": default, "record_offset": offset, "value_offset": offset + 28,
                       "value_length": value_length, "record_length": record_length})
        offset += record_length
    if (len(fields) != spec["count"] or offset != spec["size"] or
            len({f["name"] for f in fields}) != len(fields) or
            [f["name"] for f in fields if f["primary"]] != ["ViewName"] or
            fields[0]["name"] != "ViewName" or fields[0]["type"] != 2):
        raise ValueError("incorrect policy layout/key/count")
    return {"view": table_name, "size": offset, "max_rows": spec["max_rows"],
            "registration_arg5": 1024, "view_flags": 63, "source_sha256": CSPD_SHA256,
            "fields": fields, "controller_ready": False, "wlan_profile_ready": False}


def policy_constructor_fields(path, table_name):
    if table_name not in POLICY_TABLES:
        raise ValueError("unknown policy table")
    image, table, domains = constructor_data(path, table_name)
    spec = POLICY_TABLES[table_name]
    expected_table = bytearray(164)
    name_ptr = struct.unpack_from("<I", table)[0]
    struct.pack_into("<III", expected_table, 0, name_ptr, 0, spec["max_rows"])
    expected_table[12] = 1
    if expected_table != table or image.string(name_ptr) != table_name:
        raise ValueError("policy table descriptor mismatch")
    fields = []
    for index in range(spec["count"] + 1):
        data = domains[index * 68:(index + 1) * 68]
        name_ptr, _, dtype = struct.unpack_from("<III", data)
        default_ptr = struct.unpack_from("<I", data, 24)[0]
        name = image.string(name_ptr)
        if index == spec["count"]:
            if name != "":
                raise ValueError("policy terminator missing")
            break
        expected = bytearray(68)
        struct.pack_into("<III", expected, 0, name_ptr, 0, dtype)
        expected[12:14] = data[12:14]
        struct.pack_into("<I", expected, 24, default_ptr)
        if expected != data or data[12] not in (0, 1) or data[13] not in (0, 1) or dtype not in TYPE_LENGTHS:
            raise ValueError("policy domain bounds/flags/ABI mismatch")
        default = image.string(default_ptr)
        if dtype in (2, 3, 5, 7) and default:
            # Do not copy, hash, log or export any original string default.
            raise ValueError("nonempty source policy string default forbidden")
        fields.append({"name": name, "type": dtype, "primary": data[12],
                       "save": data[13], "default": default})
    return fields


def verify_policy_registration(path, table_name):
    """Verify the exact _dbInitTbl argument setup, without executing defaults."""
    if table_name not in POLICY_TABLES:
        raise ValueError("unknown policy table")
    image = Image(path, CSPD_SHA256)
    start = {"MultiAPMaster": 0x1f27e8, "MultiAPDomain": 0x1f348c}[table_name]
    fixed = {0x00: 0xe92d4830, 0x04: 0xe3a05000, 0x08: 0xe59f4054,
             0x18: 0xe3a0303f, 0x1c: 0xe59f2044, 0x20: 0xe1a01004,
             0x24: 0xe58d3008, 0x28: 0xe3a03b01, 0x2c: 0xe58d2014,
             0x30: 0xe1a00004, 0x34: 0xe58d2010, 0x38: 0xe88d0028,
             0x40: 0xe58d400c}
    for off, expected in fixed.items():
        if struct.unpack("<I", image.read(start + off, 4))[0] != expected:
            raise ValueError("policy view registration instruction mismatch")
    size_word = struct.unpack("<I", image.read(start + 0x3c, 4))[0]
    if (size_word & 0xfffff000 != 0xe3003000 or
            (((size_word >> 4) & 0xf000) | (size_word & 0xfff)) != POLICY_TABLES[table_name]["size"]):
        raise ValueError("policy view registration size mismatch")
    branch = struct.unpack("<I", image.read(start + 0x44, 4))[0]
    delta = branch & 0xffffff
    if delta & 0x800000:
        delta -= 1 << 24
    if branch >> 24 != 0xeb or start + 0x4c + delta * 4 != 0x1ebcc:
        raise ValueError("policy view registration call mismatch")
    name, empty = struct.unpack("<II", image.read(start + 0x64, 8))
    if image.string(name) != table_name or image.string(empty) != "":
        raise ValueError("policy view registration name/selector mismatch")


def verify_policy_constructor(path, schema):
    verify_policy_registration(path, schema["view"])
    actual = policy_constructor_fields(path, schema["view"])
    keys = ("name", "type", "primary", "save", "default")
    expected = [{k: f[k] for k in keys} for f in schema["fields"]]
    if actual != expected:
        raise ValueError("policy registry differs from pinned constructor")


def emit_policy_definitions(path, directory):
    """Explicit offline generation; build verifies but never rewrites sources."""
    for name, spec in POLICY_TABLES.items():
        fields = policy_constructor_fields(path, name)
        lines = [f"/* {name}: hash-pinned registry defaults, not cspd runtime defaults.",
                 f" * Source SHA256: {CSPD_SHA256}", " * All string registration defaults are empty; no credentials.", " */"]
        for f in fields:
            lines.append('MESH_FIELD(%s, %d, %d, %d, %s)' % (
                json.dumps(f["name"]), f["type"], f["primary"], f["save"], json.dumps(f["default"])))
        destination = directory / spec["file"]
        if destination.exists():
            raise ValueError("refusing to overwrite an existing registry")
        destination.write_text("\n".join(lines) + "\n")


def verify_lengths(path):
    image = Image(path, DB_SHA256)
    values = struct.unpack("<52H", image.read(0x103d2, 104))
    for dtype, lengths in TYPE_LENGTHS.items():
        if (values[dtype], values[26 + dtype]) != lengths:
            raise ValueError("native DB type-length table mismatch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-cspd", type=Path)
    parser.add_argument("--verify-libdb", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--policies-output", type=Path)
    parser.add_argument("--emit-policy-dir", type=Path)
    args = parser.parse_args()
    if args.emit_policy_dir:
        if not args.verify_cspd:
            parser.error("policy generation requires the pinned cspd evidence")
        emit_policy_definitions(args.verify_cspd, args.emit_policy_dir)
    schema = registry()
    policies = [policy_registry(name) for name in POLICY_TABLES]
    if args.verify_cspd:
        verify_constructor(args.verify_cspd, schema)
        for policy in policies:
            verify_policy_constructor(args.verify_cspd, policy)
    if args.verify_libdb:
        verify_lengths(args.verify_libdb)
    if args.output:
        args.output.write_text(json.dumps(schema, indent=2, sort_keys=True) + "\n")
    if args.policies_output:
        args.policies_output.write_text(json.dumps(policies, indent=2, sort_keys=True) + "\n")
    print("Mesh registry: Cfg 50/2952, Master 55/2312, Domain 106/3464; Controller remains unverified")


if __name__ == "__main__":
    main()
