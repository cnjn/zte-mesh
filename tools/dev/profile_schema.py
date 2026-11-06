#!/usr/bin/env python3
"""Hash-pinned WLAN/identity metadata; NEVER read any source default value.

Hardware-dependent constructor calls are data-only models. The source cspd,
its hardware/default functions, and getters with setters/CSP are never run.
The defaults below are independent public zeros, not recovered factory data.
"""
import argparse
import json
import re
import struct
from pathlib import Path

from schema import CSPD_SHA256, DB_SHA256, Image, _decode_constructor

TYPES = {0: (12, 40), 1: (20, 48), 2: (36, 64), 3: (68, 96),
         5: (260, 288), 7: (1028, 1056), 12: (4, 32), 13: (4, 32),
         14: (4, 32), 15: (1, 32), 16: (4, 32), 18: (6, 36), 22: (8, 36)}
SPECS = {
    "WLANBase": {"file": "wlan_base.def", "count": 70, "size": 2576,
                 "ctor": (0x1ecd5c, 0x1ecdac, 4828), "fixture_rows": 2,
                 "dimension": (0x1ecd88, (1, 2)), "flags": []},
    "WLANCfg": {"file": "wlan_cfg.def", "count": 53, "size": 2312,
                "ctor": (0x1ecfe0, 0x1ed4d0, 3672), "fixture_rows": 32,
                "dimension": (0x1ed48c, (4, 8)),
                "flags": [(0x1ed4bc, "ESSID", 2), (0x1ed4cc, "WPAEAPSecret", 1)]},
    "WLANSSID": {"file": "wlan_ssid.def", "count": 8, "size": 712,
                 "ctor": (0x1edcd0, 0x1edde4, 612), "fixture_rows": 32,
                 "dimension": (0x1eddc8, (4, 8)), "flags": []},
    "WLANPSK": {"file": "wlan_psk.def", "count": 4, "size": 328,
                "ctor": (0x1ed850, 0x1ed958, 340), "fixture_rows": 32,
                "dimension": (0x1ed904, (4, 8)),
                "hardware": 0x1ed920,
                "flags": [(0x1ed944, "PreSharedKey", 1), (0x1ed954, "KeyPassphrase", 1)]},
    "DevInfo": {"file": "dev_info.def", "count": 45, "size": 6024,
                "ctor": (0x1d0728, 0x1d0af0, 3128), "fixture_rows": 1, "flags": []},
}
PATTERN = re.compile(r'^PROFILE_FIELD\("([A-Za-z0-9_]+)", (\d+), ([01]), ([01]), "([^"\\]*)"\)$')


def public_default(dtype):
    if dtype < 12:
        return ""
    if dtype == 16:
        return "0.0.0.0"
    if dtype == 18:
        return "00:00:00:00:00:00"
    if dtype in TYPES:
        return "0"
    raise ValueError("unsupported profile type")


def recover_fields(path, name):
    if name not in SPECS:
        raise ValueError("unknown profile table")
    spec = SPECS[name]
    models, seen = {}, set()
    if "dimension" in spec:
        pc, args = spec["dimension"]
        def dimension(regs, recovered, image):
            if tuple(regs[:2]) != args:
                raise ValueError("profile dimension call arguments mismatch")
            # A fixture bound only; do not claim to recover runtime radio limits.
            regs[0] = spec["fixture_rows"]
            seen.add("dimension")
        models[pc] = (0x1fcac, dimension)
    if "hardware" in spec:
        def hardware(regs, recovered, image):
            if regs[2] != 0x50100a or regs[3] != 65:
                raise ValueError("PSK hardware-default call mismatch")
            # No default pointer is dereferenced; no hardware call or memory write.
            regs[0] = 0
            seen.add("hardware")
        models[spec["hardware"]] = (0x1dd50, hardware)
    for pc, field, flag in spec["flags"]:
        def flag_model(regs, recovered, image, field=field, flag=flag):
            if (image.string(regs[0]) != name or image.string(regs[1]) != field or regs[2] != flag):
                raise ValueError("profile domain flag call mismatch")
            regs[0] = 0
            seen.add(field)
        models[pc] = (0x1ecb0, flag_model)
    image, table, domains = _decode_constructor(path, spec["ctor"], models)
    required = {f for _, f, _ in spec["flags"]}
    required.update(k for k in ("dimension", "hardware") if k in spec)
    if seen != required:
        raise ValueError("profile constructor models incomplete")
    name_ptr = struct.unpack_from("<I", table)[0]
    expected = bytearray(164)
    struct.pack_into("<III", expected, 0, name_ptr, 0, spec["fixture_rows"])
    expected[12] = 0 if name == "WLANSSID" else 1
    if table != expected or image.string(name_ptr) != name:
        raise ValueError("profile table metadata mismatch")
    fields = []
    for i in range(spec["count"] + 1):
        row = domains[i * 68:(i + 1) * 68]
        ptr, _, dtype = struct.unpack_from("<III", row)
        label = image.string(ptr)
        if i == spec["count"]:
            if label != "":
                raise ValueError("profile terminator missing")
            break
        expected = bytearray(68)
        struct.pack_into("<III", expected, 0, ptr, 0, dtype)
        expected[12:14] = row[12:14]
        # Compare the pointer bytes structurally but NEVER read their contents.
        expected[24:28] = row[24:28]
        if (expected != row or row[12] not in (0, 1) or row[13] not in (0, 1)
                or dtype not in TYPES or not re.fullmatch(r"[A-Za-z0-9_]{1,32}", label)):
            raise ValueError("profile domain ABI/bounds/flags mismatch")
        fields.append({"name": label, "type": dtype, "primary": row[12], "save": row[13],
                       "default": public_default(dtype)})
    return fields


def registry(name, path=None):
    if name not in SPECS:
        raise ValueError("unknown profile table")
    spec = SPECS[name]
    path = path or Path(__file__).with_name(spec["file"])
    fields, offset = [], 8
    for line in path.read_text().splitlines():
        if not line.startswith("PROFILE_FIELD"):
            continue
        m = PATTERN.fullmatch(line)
        if not m:
            raise ValueError("malformed profile registry")
        label, dtype, primary, saved, default = m.groups()
        dtype, primary, saved = int(dtype), int(primary), int(saved)
        if len(label) > 32 or dtype not in TYPES or default != public_default(dtype):
            raise ValueError("non-public default or unsupported profile metadata")
        length, record = TYPES[dtype]
        fields.append({"name": label, "type": dtype, "primary": primary, "save": saved,
                       "default": default, "record_offset": offset,
                       "value_offset": offset + 28, "value_length": length, "record_length": record})
        offset += record
    keys = [f["name"] for f in fields if f["primary"]]
    if (len(fields) != spec["count"] or offset != spec["size"]
            or len({f["name"] for f in fields}) != len(fields)
            or keys != ([] if name == "DevInfo" else ["ViewName"])):
        raise ValueError("profile layout/key/count mismatch")
    return {"view": name, "size": offset, "fields": fields,
            "fixture_max_rows": spec["fixture_rows"], "defaults_policy": "public-zeros-source-not-read",
            "runtime_radio_limits_recovered": False, "source_sha256": CSPD_SHA256,
            "controller_ready": False, "wlan_profile_ready": False}


def verify(path, name):
    actual = recover_fields(path, name)
    wanted = [{k: f[k] for k in ("name", "type", "primary", "save", "default")}
              for f in registry(name)["fields"]]
    if actual != wanted:
        raise ValueError("profile registry differs from pinned metadata")


def verify_lengths(path):
    image = Image(path, DB_SHA256)
    v = struct.unpack("<52H", image.read(0x103d2, 104))
    if any((v[t], v[26 + t]) != sizes for t, sizes in TYPES.items()):
        raise ValueError("profile type-length table mismatch")


def emit(path, directory):
    destinations = [directory / s["file"] for s in SPECS.values()]
    if any(p.exists() for p in destinations):
        raise ValueError("refusing to overwrite a profile registry")
    # Decode everything before creating files; never partially emit failed evidence.
    decoded = [(n, recover_fields(path, n)) for n in SPECS]
    for (name, fields), dest in zip(decoded, destinations):
        lines = [f"/* {name}: hash-pinned metadata, independent PUBLIC zero defaults.",
                 " * NO source default/credential/serial value was read or copied.",
                 f" * Source SHA256: {CSPD_SHA256}", " */"]
        lines.extend('PROFILE_FIELD(%s, %d, %d, %d, %s)' % (
            json.dumps(f["name"]), f["type"], f["primary"], f["save"], json.dumps(f["default"])) for f in fields)
        with dest.open("x") as f:
            f.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-cspd", type=Path, required=True)
    parser.add_argument("--verify-libdb", type=Path, required=True)
    parser.add_argument("--emit-dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.emit_dir:
        emit(args.verify_cspd, args.emit_dir)
    for name in SPECS:
        verify(args.verify_cspd, name)
    verify_lengths(args.verify_libdb)
    if args.output:
        args.output.write_text(json.dumps([registry(n) for n in SPECS], indent=2, sort_keys=True) + "\n")
    print("WLAN/identity metadata: 5 tables, 180 fields; source defaults NOT read; Controller unverified")


if __name__ == "__main__":
    main()
