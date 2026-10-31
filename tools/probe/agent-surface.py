#!/usr/bin/env python3
"""Decide, from the pinned vendor binaries, what a Slave actually needs.

The controller side of this project needed two CM read stubs because the
sandbox has no radio. A Slave is the opposite: it must apply an M2 to the
local radios, which on the factory box is cspd's job. The question that sets
the cost of that work is:

    after _acdcRcvApAutoConfigWSCM2 -> acdcParseM2Msg -> acdcConfigM2EncryptedSettings,
    does the daemon (a) write the database through the CmWLANSet* family, or
    (b) only ASEND the configuration to cspd.wlan_manage.wlan_mgr?

(a) means a WLANCfg -> hostapd syncer plus one return-value shim is enough.
(b) means a wlan_mgr event translator has to be written.

Answer as of the pinned runtime: (a). The setters write WlanCfg/WlanPSK and
only then notify the management module, and the notification is the value the
setter returns, so a missing cspd turns a written configuration into a
reported failure unless CmSendMsg2MM is stubbed.

Read-only static analysis; nothing is executed, nothing is written.

Usage:
  python3 tools/probe/agent-surface.py              # verdict + evidence
  python3 tools/probe/agent-surface.py --trace      # + per-function call lists
  python3 tools/probe/agent-surface.py --elf PATH   # another multiapd build
"""
import argparse
import re
import struct
import sys
from pathlib import Path

SHDR = struct.Struct("<IIIIIIIIII")
SYM = struct.Struct("<IIIBBH")
REL = struct.Struct("<II")

M2_PATH = [
    "_acdcRcvApAutoConfigWSCM2",
    "acdcParseM2Msg",
    "acdcConfigM2EncryptedSettings",
    "acdcWlanSetPWKey",
    "MeshWlanSetAPCfgByBandAndIfIndex",
    "MeshWlanMapSsidCfgSync2Repeater",
    "slave_set_radio_cfg",
    "slave_init",
    "multi_ap_slave_main",
]
SETTERS = [
    # multiapd -> libcmapi entry points seen in the M2 path
    ("MeshWlanSetAPCfgByBandAndIfIndex", "CmWLANSetAPConfig"),
    ("acdcWlanSetPWKey", None),  # wrapper only; its callee is MeshWlanSetPWKeyByBandAndIfIndex
]
# Setter implementations to walk inside libcmapi.
CMAPI_ENTRY = ["CmWLANSetAPConfig", "CmWLANSetPsk"]
ORDER_KEYS = ("DbWrite", "DbRead", "cm_wlan_send", "RenewNotify", "MmSend", "2MM")


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        d = self.data
        shoff = struct.unpack_from("<I", d, 0x20)[0]
        shents = struct.unpack_from("<H", d, 0x2E)[0]
        shnum = struct.unpack_from("<H", d, 0x30)[0]
        shstrndx = struct.unpack_from("<H", d, 0x32)[0]
        self.sh = [SHDR.unpack_from(d, shoff + i * shents) for i in range(shnum)]
        self.names = {}
        for i, s in enumerate(self.sh):
            n = self._str(s[0], self.sh[shstrndx][4])
            if n:
                self.names[n] = i
        self._functions()
        self._plt()

    def _str(self, off, table):
        if not off:
            return ""
        s = table + off
        return self.data[s:self.data.index(b"\0", s)].decode("utf-8", "replace")

    def sec(self, name):
        return self.sh[self.names[name]]

    def _functions(self):
        sym = self.sec(".symtab")
        strtab = self.sh[sym[6]][4]
        out = []
        for i in range(sym[5] // SYM.size):
            name, val, size, info, _other, shndx = SYM.unpack_from(self.data, sym[4] + i * SYM.size)
            if (info & 0xF) == 2 and val and size and shndx:
                out.append((val, size, self._str(name, strtab)))
        out.sort()
        self.funcs = out
        self.by_name = {n: (v, s, n) for v, s, n in out}

    def func_at(self, addr):
        lo, hi, best = 0, len(self.funcs) - 1, None
        while lo <= hi:
            mid = (lo + hi) // 2
            if self.funcs[mid][0] <= addr:
                best, lo = self.funcs[mid], mid + 1
            else:
                hi = mid - 1
        if not best or not (best[0] <= addr < best[0] + best[1]):
            return None
        return best

    def _plt(self):
        self.plt_addr = self.sec(".plt")[3]
        dyn = self.sec(".dynsym")
        dstr = self.sh[dyn[6]][4]
        self.dynsyms = []
        for i in range(dyn[5] // SYM.size):
            name = SYM.unpack_from(self.data, dyn[4] + i * SYM.size)[0]
            self.dynsyms.append(self._str(name, dstr) if name else "")
        self.rel = self.sec(".rel.plt")
        self.plt_count = self.rel[5] // REL.size
        self.exports = set()
        for i in range(dyn[5] // SYM.size):
            _name, _val, _size, _info, _other, shndx = SYM.unpack_from(self.data, dyn[4] + i * SYM.size)
            if shndx:
                self.exports.add(self.dynsyms[i] or "")

    def plt_name(self, addr):
        idx = (addr - self.plt_addr - 20) // 12
        if 0 <= idx < self.plt_count:
            _off, info = REL.unpack_from(self.data, self.rel[4] + idx * REL.size)
            sym = info >> 8
            if sym < len(self.dynsyms):
                return self.dynsyms[sym] or None
        return None

    def offset(self, addr):
        text = self.sec(".text")
        return text[4] + (addr - text[3])

    def _decode(self, fn):
        start, size, _name = fn
        base = self.offset(start)
        for i in range(0, size - 3, 4):
            word = struct.unpack_from("<I", self.data, base + i)[0]
            if (word >> 28) != 0xE or ((word >> 24) & 0xF) not in (0xA, 0xB):
                continue
            imm = word & 0xFFFFFF
            if imm & 0x800000:
                imm -= 0x1000000
            yield i, start + i + 8 + imm * 4

    def calls(self, fn):
        seen = {}
        for _i, target in self._decode(fn):
            sym = self.plt_name(target)
            if sym:
                key = f"{sym} [plt]"
            else:
                f = self.func_at(target)
                key = f[2] if f else f"0x{target:x}"
            seen[key] = seen.get(key, 0) + 1
        return seen

    def ordered(self, fn):
        out = []
        for i, target in self._decode(fn):
            name = self.plt_name(target)
            if not name:
                f = self.func_at(target)
                name = f[2] if f and f[0] == target else None
            if name:
                out.append((fn[0] + i, name))
        return out

    def asend_events(self, fn):
        """Immediate event numbers loaded into r0 before an ASEND call."""
        start, size, _name = fn
        base = self.offset(start)
        events, r0 = [], None
        for i in range(0, size - 3, 4):
            word = struct.unpack_from("<I", self.data, base + i)[0]
            if (word & 0xFFF00000) == 0xE3000000:  # movw r0
                if (word >> 12) & 0xF == 0:
                    r0 = ((word >> 16) & 0xF) << 12 | (word & 0xFFF)
            elif (word & 0xFFF00000) == 0xE3400000:  # movt r0
                if (word >> 12) & 0xF == 0 and r0 is not None:
                    r0 = (r0 & 0xFFFF) | ((((word >> 16) & 0xF) << 12 | (word & 0xFFF)) << 16)
            elif (word & 0xFFFFF000) == 0xE59F0000:  # ldr r0, [pc, #imm]
                if (word >> 12) & 0xF == 0:
                    lit = start + i + 8 + (word & 0xFFF)
                    if self.offset(lit) + 4 <= len(self.data):
                        r0 = struct.unpack_from("<I", self.data, self.offset(lit))[0]
            if (word >> 28) == 0xE and ((word >> 24) & 0xF) in (0xA, 0xB):
                imm = word & 0xFFFFFF
                if imm & 0x800000:
                    imm -= 0x1000000
                if self.plt_name(start + i + 8 + imm * 4) == "ASEND":
                    events.append(r0)
        return events


def repo_root():
    return Path(__file__).resolve().parents[2]


def locate(repo):
    m = repo / "out/vendor/root/bin/multiapd"
    c = repo / "out/vendor/root/lib/libcmapi.so"
    if m.exists() and c.exists():
        return m, c
    tar = repo / "openwrt/zte-mesh/zte-mesh-runtime.tar.gz"
    sys.exit(
        "vendor binaries not extracted; run:\n"
        f"  tar -xzf {tar} -C {repo}/out/vendor root/bin/multiapd root/lib/libcmapi.so"
    )


def sha256(path):
    import hashlib

    return hashlib.sha256(path.read_bytes()).hexdigest()


def phase_gate(elf, trace):
    print("== 1. gate: can the agent module initialise at all? ==")
    for name in ("multi_ap_slave_main", "slave_init"):
        fn = elf.by_name.get(name)
        if not fn:
            print(f"  {name}: absent")
            continue
        print(f"  {name} @0x{fn[0]:x} ({fn[1]} bytes)")
        if trace:
            for k, c in sorted(elf.calls(fn).items()):
                print(f"     {c}x {k}")
    main = elf.by_name.get("multi_ap_slave_main")
    if main:
        for i, target in elf._decode(main):
            f = elf.func_at(target)
            if f and f[2] == "slave_init":
                print(f"  slave_init is called at 0x{main[0] + i:x}, behind a data byte, not a build-time switch")
                break
    print()


def phase_m2(elf, trace):
    print("== 2. where an M2 lands: direct calls in the receive path ==")
    for name in M2_PATH:
        fn = elf.by_name.get(name)
        if not fn:
            print(f"  {name}: absent")
            continue
        calls = elf.calls(fn)
        print(f"  {name} @0x{fn[0]:x}")
        if trace:
            for k, c in sorted(calls.items()):
                print(f"     {c}x {k}")
        else:
            keep = [k for k in calls if re.match(r"^(Cm|Mesh|acdc|slave_init|multiap_set|dbAPI)", k)]
            print(f"     -> {', '.join(sorted(keep)) if keep else '(no platform calls)'}")
        if "ASEND [plt]" in calls:
            print(f"     ASEND events: {elf.asend_events(fn) or 'unresolved'}")
    print()


def phase_setter(elf, cm):
    print("== 3. does the setter write the database before notifying cspd? ==")
    verdict = None
    for entry in CMAPI_ENTRY:
        fn = cm.by_name.get(entry)
        if not fn:
            print(f"  {entry}: absent in libcmapi")
            continue
        # The exported entry is a thin wrapper; the work lives in a local _name.
        impls = [fn]
        inner = cm.by_name.get("_" + entry)
        if inner:
            impls.insert(0, inner)
        for impl in impls:
            seq = [(a, n) for a, n in cm.ordered(impl) if any(k in n for k in ORDER_KEYS)]
            if not seq:
                continue
            print(f"  {impl[2]} @0x{impl[0]:x} (reached from {entry})")
            for addr, n in seq:
                print(f"    0x{addr:x}  {n}")
            write = next((a for a, n in seq if "DbWrite" in n), None)
            notify = next((a for a, n in seq if "cm_wlan_send" in n), None)
            if write and notify:
                verdict = write < notify
                print(f"    write at 0x{write:x} precedes notify at 0x{notify:x}: {verdict}")
                after = [struct.unpack_from("<I", cm.data, cm.offset(notify + k))[0] for k in (4, 8, 12)]
                print(f"    after the notify call: {[f'{w:08x}' for w in after]}")
            break

    print()
    print("== 4. what the setter returns, and whether a stub is needed ==")
    fn = cm.by_name.get("CmSendMsg2MM")
    if fn:
        print(f"  CmSendMsg2MM @0x{fn[0]:x} (exported: {'yes' if 'CmSendMsg2MM' in cm.exports else 'no'})")
        for k, c in sorted(cm.calls(fn).items()):
            print(f"    {c}x {k}")
        inner = cm.by_name.get("CmSendMsg2MMWithTimerOut")
        if inner:
            calls = cm.calls(inner)
            print(f"  CmSendMsg2MMWithTimerOut: {'SSEND [plt]' in calls and 'synchronous send, blocks to timeout when cspd is absent' or 'no SSEND'}")
    return verdict


def phase_imports(elf):
    print("== 5. platform surface the agent side imports ==")
    seen = {}
    for name in M2_PATH:
        fn = elf.by_name.get(name)
        if not fn:
            continue
        for k, c in elf.calls(fn).items():
            if k.endswith("[plt]"):
                seen[k] = seen.get(k, 0) + c
    for k in sorted(seen):
        print(f"  {seen[k]}x {k}")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", type=Path, default=None, help="multiapd to analyse (default: out/vendor)")
    ap.add_argument("--trace", action="store_true", help="print every direct call instead of the summary")
    args = ap.parse_args()

    repo = repo_root()
    multiapd, cmapi = locate(repo)
    if args.elf:
        multiapd = args.elf
    elf = Elf(multiapd)
    cm = Elf(cmapi)
    print(f"multiapd  {sha256(multiapd)}  <{multiapd}>")
    print(f"libcmapi  {sha256(cmapi)}  <{cmapi}>")
    print(f"multiapd functions with code: {len(elf.funcs)}; libcmapi: {len(cm.funcs)}\n")

    phase_gate(elf, args.trace)
    phase_m2(elf, args.trace)
    wrote_first = phase_setter(elf, cm)
    phase_imports(elf)

    print("== verdict ==")
    if wrote_first:
        print("  The Slave applies an M2 through libcmapi's CmWLANSet* family, which writes")
        print("  WlanCfg/WlanPSK into the shared database before it notifies the management")
        print("  module. With no cspd the write still happens and only the notification fails,")
        print("  and that failure is what the setter returns, so the agent reports a config")
        print("  failure unless CmSendMsg2MM is stubbed to succeed.")
        print("  => the missing piece is a database -> hostapd syncer plus a one-symbol")
        print("     return-value shim, not a wlan_mgr event translator.")
    else:
        print("  Could not establish the write-before-notify order; treat the cost as unknown.")


if __name__ == "__main__":
    main()
