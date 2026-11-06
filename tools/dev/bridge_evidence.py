#!/usr/bin/env python3
"""Verify only public WLAN enum tables supporting the bridge's one mode."""
import argparse
import json
import re
import struct
from pathlib import Path
from schema import Image

CFAPI_SHA256 = "66b4f5a8e3761e82a13960718733c45e076decdfd6b560af844782239d10b099"
TABLES = {
    "beacon": (0x77868, 112, ("11i", 3)),
    "auth": (0x78790, 40, ("PSKAuthentication", 0)),
    "encryption": (0x77848, 32, ("AESEncryption", 1)),
}


def verify(path):
    image = Image(path, CFAPI_SHA256)
    for address, size, wanted in TABLES.values():
        found = []
        for off in range(0, size, 8):
            ptr, value = struct.unpack("<II", image.read(address + off, 8))
            if ptr:
                found.append((image.string(ptr), value))
            elif off != size - 8 or value:
                raise ValueError("invalid public enum terminator")
        if found.count(wanted) != 1:
            raise ValueError("bridge public enum mismatch")
    header = Path(__file__).with_name("mesh-bridge.h").read_text()
    expected = {"MB_VENDOR_BEACON_WPA2": 3, "MB_VENDOR_AUTH_PSK": 0, "MB_VENDOR_CIPHER_AES": 1}
    for name, value in expected.items():
        if not re.search(r"^#define " + name + r" " + str(value) + r"U$", header, re.M):
            raise ValueError("bridge compiled enum constant mismatch")
    return {"source_sha256": CFAPI_SHA256, "mode": "wpa2-psk-aes",
            "fields": {"BeaconType": 3, "11iAuthMode": 0, "11iEncryptType": 1},
            "hardware_verified": False, "controller_ready": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libcfapi", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify(args.libcfapi)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print("Bridge PUBLIC enum tuple verified: 11i / PSK / AES; Controller unverified")
