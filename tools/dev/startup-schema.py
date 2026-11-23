#!/usr/bin/env python3
"""Only metadata for views actually blocking bootstrap/configuration.
Never dereference source default pointers; all defaults are public zeros.
"""
import argparse
import json
import struct
from pathlib import Path
from schema import _decode_constructor, CSPD_SHA256, DB_SHA256, Image
from profile_schema import public_default

SPECS = {"P1905ComCfg": ((0x1f0ca4, 0x1f0dbc, 612), 8, 1, 328),
         "IPInterface": ((0x1db3a0, 0x1db800, 3332), 48, 25, 2404),
         "P1905_ACD": ((0x1f0430, 0x1f0570, 680), 9, 1, 456),
         "P1905AcdBssCfg": ((0x1f0aa4, 0x1f0b88, 408), 5, 8, 264),
         # dbCreateAclCfgTbl @0x1cd334 (MAC filter rows the controller pushes per BSS)
         "AclCfg": ((0x1cd334, 0x1cd41c, 408), 5, 256, 268),
         "WLANWEP": ((0x1ee5d4, 0x1ee6a4, 272), 3, 32, 232),
         # dbCreate1905DevInfoTbl @0x1f08ac: the node inventory the daemon fills.
         "P1905DeviceInfo": ((0x1f08ac, 0x1f09fc, 816), 11, 1024, 788),
         # dbCreateMultiAPRoamCatonCfgTbl @0x1f4260: read by MapGetRoamCatonCfgDB for the Roam Caton
         # vendor TLV (0x4f) of the first PolicyConfigReq. (0x1f4198 is the defaults setter, not read.)
         "MultiAPRoamCatonCfg": ((0x1f4260, 0x1f4368, 544), 7, 1, 232),
         # dbCreateWlanSettingTbl @0x1edc2c: the domain array lives in .data (tWlanSettingTblDm @0x2ba298);
         # read by WlanSettingDbRead (dual-band/SSID sync, ESSID suffix, MLO) for the M2 0xe0 TLV and 0200fe.
         "WLANSetting": ((0x1edc2c, 0x1edc74, 1020), 14, 1, 520),
         # dbCreateMultiAPCacheMacTbl @0x1f406c: I5TlvVendorRoamCatonTypeInsert lists it (dbAPILstView) after
         # reading MultiAPRoamCatonCfg; if the list fails it returns -8 and the whole 0x4d/0x4f/0x60 body collapses.
         "MultiAPCacheMac": ((0x1f406c, 0x1f4118, 204), 2, 200, 108)}
PRIMARY = {"IPInterface": ["ViewName"], "P1905AcdBssCfg": ["ViewName"], "AclCfg": ["ViewName"], "WLANWEP": ["ViewName"],
           "P1905DeviceInfo": ["ViewName", "MacAddr"], "WLANSetting": ["ViewName"],
           "MultiAPCacheMac": ["ViewName", "MacAddr"]}


def recover(cspd, libdb):
    db = Image(libdb, DB_SHA256)
    sizes = struct.unpack("<52H", db.read(0x103d2, 104))
    result = {}
    for name, (ctor, count, rows, byte_size) in SPECS.items():
        models, seen = {}, set()
        if name == "WLANWEP":
            def dimension(regs, recovered, image):
                if tuple(regs[:2]) != (16, 32):
                    raise ValueError("WEP dimension metadata mismatch")
                # Private fixture bound only; do not call a hardware helper.
                regs[0] = 32
                seen.add("dimension")
            def flag(regs, recovered, image):
                if (image.string(regs[0]), image.string(regs[1]), regs[2]) != ("WLANWEP", "Key", 1):
                    raise ValueError("WEP protected-field metadata mismatch")
                regs[0] = 0
                seen.add("flag")
            models = {0x1ee670: (0x1fcac, dimension), 0x1ee69c: (0x1ecb0, flag)}
        image, table, domains = _decode_constructor(cspd, ctor, models)
        if name == "WLANWEP" and seen != {"dimension", "flag"}:
            raise ValueError("incomplete WEP constructor model")
        ptr = struct.unpack_from("<I", table)[0]
        expected = bytearray(164)
        struct.pack_into("<III", expected, 0, ptr, 0, rows)
        expected[12] = 1
        if table != expected or image.string(ptr) != name:
            raise ValueError("startup table descriptor mismatch")
        fields, offset = [], 8
        for i in range(count + 1):
            row = domains[i * 68:(i + 1) * 68]
            ptr, _, dtype = struct.unpack_from("<III", row)
            label = image.string(ptr)
            if i == count:
                if label:
                    raise ValueError("startup descriptor terminator missing")
                break
            expected = bytearray(68)
            struct.pack_into("<III", expected, 0, ptr, 0, dtype)
            expected[12:14] = row[12:14]
            expected[24:28] = row[24:28]  # Pointer bytes only; never source values.
            default = "::" if dtype == 17 else public_default(dtype)
            if expected != row or row[12] not in (0, 1) or row[13] not in (0, 1):
                raise ValueError("startup field ABI mismatch")
            fields.append({"name": label, "type": dtype, "primary": row[12], "save": row[13],
                           "default": default, "value_offset": offset + 28,
                           "value_length": sizes[dtype], "record_length": sizes[26 + dtype]})
            offset += sizes[26 + dtype]
        if offset != byte_size or [f["name"] for f in fields if f["primary"]] != PRIMARY.get(name, []):
            raise ValueError("startup field layout mismatch %s offset=%d primary=%s" % (name, offset, [f["name"] for f in fields if f["primary"]]))
        result[name] = {"size": byte_size, "max_rows": rows, "fields": fields,
                        "source_sha256": CSPD_SHA256, "source_defaults_read": False}
    return result


def declaration(tables):
    lines = ["/* Fixed baseline metadata; independent public zero defaults. */"]
    for name, table in tables.items():
        lines.append("static const struct zte_db_domain startup_" + name + "[] = {")
        for f in table["fields"]:
            lines.append("    { .name = %s, .type = %d, .primary_key = %d, .save = %d, .default_value = %s }," %
                         (json.dumps(f["name"]), f["type"], f["primary"], f["save"], json.dumps(f["default"])))
        lines.extend(['    { .name = "", .default_value = "0" }', "};"])
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cspd", type=Path, required=True)
    parser.add_argument("--libdb", type=Path, required=True)
    parser.add_argument("--emit", type=Path)
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    text = declaration(recover(args.cspd, args.libdb))
    if args.emit:
        with args.emit.open("x") as f:
            f.write(text)
    if args.verify and args.verify.read_text() != text:
        raise SystemExit("startup registry differs from pinned metadata")
    print("Actual startup views verified: P1905ComCfg 328, IPInterface 2404, P1905_ACD 456, P1905AcdBssCfg 264, AclCfg 268, WLANWEP 232, P1905DeviceInfo 788, MultiAPRoamCatonCfg 232, WLANSetting 520, MultiAPCacheMac 108 bytes; source defaults not read")


if __name__ == "__main__":
    main()
