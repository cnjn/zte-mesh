#!/usr/bin/env python3
"""Scan candidate files for the AES-128-ECB key of an encrypted rootfs region.

Method (borrowed from zte-sr1010-research software-reverse phase 5):
  JFFS2 erased flash is 0xFF-filled, so under ECB the most frequent 16-byte
  ciphertext block equals AES_ECB(key, FF*16).  A candidate key is tested by
  encrypting one block; no filesystem magic needed.  Sources are scanned
  window-by-window (printable-ASCII runs by default, every offset with --full).

Usage:
  python3 tools/lab/ecb-key-scan.py --cipher FIRMWARE.bin --offset 0x340214 \
      --length 23461888 --scan boot.bin --scan kernel-Image.bin
  python3 tools/lab/ecb-key-scan.py ... --key-hex HEX --decrypt rootfs.jffs2
"""

import argparse
import sys
from collections import Counter
from pathlib import Path

# --- minimal AES-128 (encrypt + decrypt, single block) ----------------------

SBOX = [
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
]
INV_SBOX = [0] * 256
for _i, _v in enumerate(SBOX):
    INV_SBOX[_v] = _i
RCON = [0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def _xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else (a << 1) & 0xFF


def _gmul(a, b):
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        a = _xtime(a)
        b >>= 1
    return p & 0xFF


def _expand(key):
    w = list(key)
    for i in range(4, 44):
        t = w[(i - 1) * 4:i * 4]
        if i % 4 == 0:
            t = [SBOX[t[1]] ^ RCON[i // 4], SBOX[t[2]], SBOX[t[3]], SBOX[t[0]]]
        w.extend(w[(i - 4) * 4 + j] ^ t[j] for j in range(4))
    return [w[r * 16:(r + 1) * 16] for r in range(11)]


def aes128_encrypt_block(key, block):
    rk = _expand(key)
    s = [block[i] ^ rk[0][i] for i in range(16)]
    for rnd in range(1, 10):
        s = [SBOX[b] for b in s]
        s = [s[0], s[5], s[10], s[15], s[4], s[9], s[14], s[3],
             s[8], s[13], s[2], s[7], s[12], s[1], s[6], s[11]]
        m = []
        for c in range(4):
            a, b, d, e = s[4 * c:4 * c + 4]
            m += [(_xtime(a) ^ _xtime(b) ^ b ^ d ^ e),
                  (a ^ _xtime(b) ^ _xtime(d) ^ d ^ e),
                  (a ^ b ^ _xtime(d) ^ _xtime(e) ^ e),
                  (_xtime(a) ^ a ^ b ^ d ^ _xtime(e))]
        s = [m[i] ^ rk[rnd][i] for i in range(16)]
    s = [SBOX[b] for b in s]
    s = [s[0], s[5], s[10], s[15], s[4], s[9], s[14], s[3],
         s[8], s[13], s[2], s[7], s[12], s[1], s[6], s[11]]
    return bytes(s[i] ^ rk[10][i] for i in range(16))


def aes128_decrypt_block(key, block):
    rk = _expand(key)
    s = [block[i] ^ rk[10][i] for i in range(16)]
    for rnd in range(9, 0, -1):
        s = [s[0], s[13], s[10], s[7], s[4], s[1], s[14], s[11],
             s[8], s[5], s[2], s[15], s[12], s[9], s[6], s[3]]
        s = [INV_SBOX[b] for b in s]
        s = [s[i] ^ rk[rnd][i] for i in range(16)]
        m = []
        for c in range(4):
            a, b, d, e = s[4 * c:4 * c + 4]
            m += [(_gmul(0x0E, a) ^ _gmul(0x0B, b) ^ _gmul(0x0D, d) ^ _gmul(0x09, e)),
                  (_gmul(0x09, a) ^ _gmul(0x0E, b) ^ _gmul(0x0B, d) ^ _gmul(0x0D, e)),
                  (_gmul(0x0D, a) ^ _gmul(0x09, b) ^ _gmul(0x0E, d) ^ _gmul(0x0B, e)),
                  (_gmul(0x0B, a) ^ _gmul(0x0D, b) ^ _gmul(0x09, d) ^ _gmul(0x0E, e))]
        s = m
    s = [s[0], s[13], s[10], s[7], s[4], s[1], s[14], s[11],
         s[8], s[5], s[2], s[15], s[12], s[9], s[6], s[3]]
    s = [INV_SBOX[b] for b in s]
    return bytes(s[i] ^ rk[0][i] for i in range(16))


# --- scanning ---------------------------------------------------------------

def derive_oracle(cipher, offset, length):
    """Most frequent 16-byte block of the ciphertext region (region-aligned)."""
    region = cipher[offset:offset + length]
    if len(region) != length:
        raise SystemExit("cipher region out of range")
    if length % 16:
        raise SystemExit("cipher region length must be 16-aligned")
    counts = Counter(region[i:i + 16] for i in range(0, len(region), 16))
    block, n = counts.most_common(1)[0]
    return block, n


def windows(data, full):
    if full:
        for i in range(len(data) - 15):
            yield i, data[i:i + 16]
        return
    run = bytearray()
    start = 0
    for i, b in enumerate(data):
        if 0x20 <= b <= 0x7E:
            if not run:
                start = i
            run.append(b)
        else:
            for j in range(max(0, len(run) - 15)):
                yield start + j, bytes(run[j:j + 16])
            run.clear()
    for j in range(max(0, len(run) - 15)):
        yield start + j, bytes(run[j:j + 16])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cipher", type=Path, required=True, help="encrypted firmware image")
    ap.add_argument("--offset", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--length", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--scan", type=Path, action="append", default=[],
                    help="candidate source file (repeatable)")
    ap.add_argument("--full", action="store_true",
                    help="slide over every offset instead of printable-ASCII runs")
    ap.add_argument("--plain-block", default="ff" * 16,
                    help="assumed plaintext block behind the oracle (default FF*16)")
    ap.add_argument("--key-ascii", help="test one 16-char ASCII key directly")
    ap.add_argument("--key-hex", help="test one 16-byte hex key directly")
    ap.add_argument("--decrypt", type=Path, help="decrypt the region to this path")
    a = ap.parse_args()

    cipher = a.cipher.read_bytes()
    region = cipher[a.offset:a.offset + a.length]
    if len(region) != a.length or a.length % 16:
        raise SystemExit("bad cipher region")
    oracle, n = derive_oracle(cipher, a.offset, a.length)
    plain = bytes.fromhex(a.plain_block)
    print(f"oracle block (x{n}): {oracle.hex()}")
    print(f"assumed plaintext:   {plain.hex()}")

    found = []

    def test(key, where):
        if aes128_encrypt_block(key, plain) == oracle:
            found.append((key, where))
            return True
        return False

    if a.key_hex:
        key = bytes.fromhex(a.key_hex)
        if len(key) != 16:
            raise SystemExit("key must be 16 bytes")
        test(key, "--key-hex")
    if a.key_ascii:
        key = a.key_ascii.encode("ascii")
        if len(key) != 16:
            raise SystemExit("ASCII key must be 16 chars")
        test(key, "--key-ascii")

    for path in a.scan:
        data = path.read_bytes()
        tested = 0
        for pos, win in windows(data, a.full):
            tested += 1
            if test(win, f"{path.name}+0x{pos:x}"):
                print(f"HIT {path.name}+0x{pos:x} key={win.hex()} ({win!r})")
                break
        print(f"scanned {path.name}: {tested} candidates")

    if not found:
        print("no key found")
        return 1
    key, where = found[0]
    print(f"KEY {key.hex()} ascii={key!r} from {where}")
    if a.decrypt:
        out = bytearray()
        dec = aes128_decrypt_block
        for i in range(0, len(region), 16):
            out += dec(key, region[i:i + 16])
        a.decrypt.write_bytes(bytes(out))
        head = bytes(out[:16])
        print(f"decrypted {len(out)} bytes -> {a.decrypt}; head={head.hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
