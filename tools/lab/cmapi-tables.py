"""Cm* API 到底读写哪张表：从 ARM32 共享库反推「API → DB 表」映射，零依赖。

为什么需要它：把原厂 mesh 栈搬到 mac80211 平台时，唯一要新写的是「DB 表 → nl80211」
的 applier（见 docs/research/BLOB_SHIM.md §4）。写多少、覆盖哪些列，取决于
`CmWLAN*` 这一层最终落到哪几张表 —— 这个数只能从机器码里取，不能靠猜。

原理（PIC 共享库里字符串不走绝对地址，这是四次踩坑后的正解）：

  1. **字符串地址在 GOT 里**：`.rel.dyn` 的 `R_ARM_RELATIVE` 条目把「GOT 槽 → 目标 VA」
     记在槽的**初始内容**里（文件里不是 0），所以要先建这张表。
  2. **变量串是 `ldr` + `add pc` 两段式**：GCC 为省字面量池，常写
         ldr r0, =<小偏移>        ; e59f0xxx，字面量是个 0x0000b72c 这样的**偏移**
         ...（中间可能隔几条指令）
         add r0, pc, r0           ; e08f0000，真实地址 = 偏移 + 该 add 的 PC(+8)
     实测 `WlanCfgDbRead` 里 `dbAPIGetView("WLANCfg", …)` 的 r0 就是这么来的：
     `0xb72c + 0x506c4 + 8 = 0x5bdf8`，而 `0x5bdf8` 正是 `"WLANCfg"`。
     **只认「字面量 = 字符串 VA」会得到 0 命中**，这是第一版失败的原因。
  3. **表名就是 dbAPIGetView 的第一个参数**：每个 `Wlan*Db*` 函数体内只有一处
     `dbAPIGetView`，r0 即表名。所以「Db 函数 → 表」可以直接读出来，不需要逆 shm 布局。
     **不要去收集"函数里出现的所有表名字符串"** —— 日志串里也有表名，会把日志函数
     误记成读写者。
  4. **跨库边 + 间接调用**：`CmWLANGetAPConfig` 经 PLT 调 `WlanCfgDbRead`（在 `libcfapi.so`），
     图必须跨 `.so` 连；而 `CmWLANSetPsk` 这一类是 `ldr ip, =func; bx ip`（尾调用），
     只认 `bl` 会得到"可达 1 个符号"的假象。

用法:
    # 一张完整对照表：Db 函数 → 表名
    python3 tools/lab/cmapi-tables.py <libdir> --tables

    # 某个 API 的完整落点（跨库可达，深度默认 3）
    python3 tools/lab/cmapi-tables.py <libdir> --api CmWLANGetAPConfig CmWLANKickOutSta

    # multiapd 实际导入的全部 Cm*/Cf*/Wlan*（61 个）
    python3 tools/lab/cmapi-tables.py <libdir> --multiapd-api --multiapd <path>/bin/multiapd

    # 机器可读
    python3 tools/lab/cmapi-tables.py <libdir> --tables --multiapd-api --multiapd ... \
        --json out/cmapi-tables.json
"""

import argparse
import bisect
import collections
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from armcalls import Elf, find_plt_stubs, sext  # noqa: E402

DT_REL, DT_RELSZ, DT_RELENT = 17, 18, 19
R_ARM_RELATIVE = 23
LDR_PC = 0x0F3F0000, 0x051F0000      # ldr rD, [pc, #±imm]
ADD_PC_REG = 0x0FFF0FF0, 0x008F0000  # add rD, pc, rS
ADD_PC_IMM = 0x0F7F0000, 0x028F0000  # add rD, pc, #imm
MOV_REG = 0x0FFF0FF0, 0x01A00000     # mov rD, rS
BX_RM = (0x012FFF10, 0x012FFF30)     # bx Rm / blx Rm（0x012FFF1E 是 bx lr，先排除）

# 已知的 DB 表名。来源：docs/milestones/SCHEMA.md 的还原 + E2631_DB_SNAPSHOT 的
# `sendcmd 1 DB p` 实测（19 张 mesh 表 + 无线配置表）。用白名单而不是模式匹配，
# 是因为 `WLAN` 这种裸串是日志前缀、`MapAccessAttr` 是 WLANCfg 的**列名**，
# 用正则会把它们都当成表。
TABLES = {
    # 无线配置本体
    "WLANCfg", "WLANBase", "WLANPSK", "WLANSSID", "WLANSetting", "WLANWEP", "WLANWMM",
    "WLANWPS", "WLANAD", "WLANScheduler", "WLANListCfg", "WLANListBase", "WLANACL",
    "WlanTimeCfg", "STAIF", "STAProfile", "APCLI", "WLANMLOAD",
    # mesh 决策与状态
    "MultiAPCfg", "MultiAPMaster", "MultiAPDomain", "MultiAPStaCfg", "MultiAPCacheMac",
    "MultiAPRoamCatonCfg", "MultiAPPTK", "MultiAPCapability", "MultiapCapability",
    "P1905Cfg", "P1905StaBind", "P1905ComCfg", "P1905StaCfg",
    # 桥 / 端口 / 准入 / 策略
    "AclCfg", "EthVlan", "EthLink", "EthPort", "Brport", "MapCooperatorCfg",
    "WlanRoamingCfg", "WlanRoamingSta", "TimePolicy", "Scheduler",
}
# 第一个参数是视图名/表名的 DB 接口（r0 是字符串指针）。
# 读走 dbAPIGetView、写走 dbAPISetView —— 写路径漏了后者时，全部 *DbWrite 都会显示
# "视图名由参数传入"，这是第二版踩的坑。
DB_VIEW_API = ("dbAPIGetView", "dbAPISetView", "dbAPIGetVal", "dbAPISetVal",
               "dbSetVal", "dbGetVal", "dbAPIGetViewEx", "dbAPISetValByRow", "dbSetValComm")
HW_WORDS = ("ioctl", "socket", "setsockopt", "netlink", "sendto", "recvfrom", "syscall",
            "devmem", "system", "popen", "execl", "execv")
VENDOR_WORDS = ("wlan_", "csp_", "mt_", "mt76", "chip_")


class Lib:
    """一个 .so：GOT 表、字符串表、PLT 映射、导出/函数区间。"""

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.e = Elf(path)
        self.got = self._load_got()
        self.strs = self._load_strings()
        self.plt = find_plt_stubs(self.e, self.e.plt_relocs())     # stub VA -> 符号
        self.exports = {}                                          # 符号 -> VA
        for nm, va in self.e.dynsym():
            if nm and va:
                self.exports.setdefault(nm, va)
        self.funcs = sorted(set(self.exports.values()))
        self.name_at = {}
        for nm, va in self.e.dynsym():
            if nm and va:
                self.name_at.setdefault(va, nm)
        txt = self.e.section(".text")
        self.text = (txt[0], txt[0] + txt[2]) if txt else (0, 0)

    # -- 基础数据 -----------------------------------------------------

    def _load_got(self):
        """GOT 槽 -> 目标 VA。R_ARM_RELATIVE 的 addend 就存在槽的初始内容里。"""
        got = {}
        dyn = self.e.dynamic()
        if DT_REL not in dyn or DT_RELSZ not in dyn:
            return got
        off = self.e.va2off(dyn[DT_REL])
        if off is None:
            return got
        step = dyn.get(DT_RELENT, 8) or 8
        for i in range(dyn[DT_RELSZ] // step):
            r_off, r_info = struct.unpack_from("<II", self.e.d, off + i * step)
            if (r_info & 0xFF) != R_ARM_RELATIVE:
                continue
            raw = self.e.read_va(r_off, 4)
            if len(raw) == 4:
                got[r_off] = struct.unpack("<I", raw)[0]
        return got

    def _load_strings(self):
        """VA -> 字符串（NUL 结尾、可打印）。

        坑：不要用 `\\x00(.*?)\\x00` 这种正则。它会把"下一个串的前导 \\x00"一起消费掉，
        而 .rodata 里字符串是紧挨着排的 —— 结果是**每隔一个漏一个**。
        `WLANPSK` 就是这么被漏掉的（用 re.search 单独搜能找到，批量遍历找不到）。
        这里用"可打印段 + 检查前后确实是 NUL"的写法，不消费分隔符。
        """
        out = {}
        d = self.e.d
        for m in re.finditer(rb"[\x20-\x7e]{2,}", d):
            s, e = m.start(), m.end()
            if s > 0 and d[s - 1] != 0:
                continue
            if e < len(d) and d[e] != 0:
                continue
            va = self.e.off2va(s)
            if va is not None:
                out[va] = m.group().decode("ascii", "replace")
        return out

    def string_at(self, va):
        """精确命中；否则取"落在某串内部"的后缀（sprintf 常指向格式串中段）。"""
        if va in self.strs:
            return self.strs[va]
        best = None
        for sva, s in self.strs.items():
            if sva < va < sva + len(s):
                if best is None or sva > best[0]:
                    best = (sva, s[va - sva:])
        return best[1] if best else None

    def owner(self, va):
        i = bisect.bisect_right(self.funcs, va) - 1
        return self.funcs[i] if i >= 0 else None

    def span(self, ent, cap=0x4000):
        nxt = [v for v in self.funcs if v > ent]
        return min(nxt[0] if nxt else ent + cap, ent + cap)

    def in_text(self, va):
        return self.text[0] <= va < self.text[1]

    # -- 指令扫描 -----------------------------------------------------

    def scan(self, ent, end):
        """扫一个函数区间，返回结构化调用点。

        {"calls": [{"va","sym","lib","inner","arg0","depth_hint"}], "callees": set}

        `arg0` 是调用发生那一刻 r0 指向的字符串（只对 DB_VIEW_API 有意义）。
        寄存器传播只跟四类写法（ldr 取字面量 / add pc / mov 传值 / bx·blx 间接跳转），
        其余写 rD 的指令一律清空该寄存器 —— 宁可漏，不要猜。
        """
        lo, hi = self.e.va2off(ent), self.e.va2off(end)
        if lo is None or hi is None:
            return {"calls": [], "callees": set()}
        ins = [(ent + (p - lo), struct.unpack_from("<I", self.e.d, p)[0])
               for p in range(lo, hi - 4, 4)]
        calls, callees = [], set()
        reg = {}

        def emit(va, target, kind):
            sym = self.plt.get(target) if kind == "plt" else self.name_at.get(target)
            arg0 = None
            if kind == "plt" and sym in DB_VIEW_API and 0 in reg:
                s = self.string_at(reg[0])
                if s and all(32 <= ord(c) < 127 for c in s):
                    arg0 = s
            calls.append({"va": va, "target": target, "sym": sym,
                          "inner": kind == "inner", "arg0": arg0})
            if kind == "inner":
                callees.add(target)
            for r in (0, 1, 2, 3, 12):
                reg.pop(r, None)

        for va, w in ins:
            if (w & 0x0F000000) in (0x0B000000, 0x0A000000):        # bl / b
                tgt = va + 8 + (sext(w & 0xFFFFFF, 24) << 2)
                emit(va, tgt, "plt" if tgt in self.plt else "inner")
                continue
            if (w & 0x0FFFFFF0) in BX_RM:                           # blx Rm / bx Rm
                rm = w & 0xF
                if rm == 14:                                        # bx lr = 返回
                    reg.clear()
                    continue
                tgt = reg.get(rm)
                if tgt and (tgt in self.plt or self.in_text(tgt)):
                    emit(va, tgt, "plt" if tgt in self.plt else "inner")
                else:
                    reg.clear()
                continue
            if (w & LDR_PC[0]) == LDR_PC[1]:                        # ldr rD, [pc, #±]
                rd = (w >> 12) & 0xF
                up = (w >> 23) & 1
                imm = w & 0xFFF
                lit = va + 8 + (imm if up else -imm)
                raw = self.e.read_va(lit, 4)
                if len(raw) == 4:
                    val = struct.unpack("<I", raw)[0]
                    reg[rd] = self.got.get(val, val)                 # GOT 解引用
                else:
                    reg.pop(rd, None)
                continue
            if (w & ADD_PC_REG[0]) == ADD_PC_REG[1]:                # add rD, pc, rS
                rd, rs = (w >> 12) & 0xF, w & 0xF
                if rs != 15 and rs in reg and isinstance(reg[rs], int):
                    reg[rd] = reg[rs] + va + 8
                else:
                    reg.pop(rd, None)
                continue
            if (w & ADD_PC_IMM[0]) == ADD_PC_IMM[1]:                # add rD, pc, #imm
                imm = w & 0xFFF
                rot = ((w >> 8) & 0xF) * 2
                if rot:
                    imm = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF
                reg[(w >> 12) & 0xF] = va + 8 + imm
                continue
            if (w & MOV_REG[0]) == MOV_REG[1]:                      # mov rD, rS
                rd, rs = (w >> 12) & 0xF, w & 0xF
                if rs in reg:
                    reg[rd] = reg[rs]
                else:
                    reg.pop(rd, None)
                continue
            if (w & 0x0C000000) == 0x04000000:                      # ldr/str 族
                if w & 0x00100000:                                  # L 位＝加载
                    reg.pop((w >> 12) & 0xF, None)
                continue
            if (w & 0x0C000000) == 0x00000000:                      # 数据处理，写 rD
                reg.pop((w >> 12) & 0xF, None)
                continue
        return {"calls": calls, "callees": callees}


def load_dir(libdir, want=None):
    libs = {}
    for f in sorted(os.listdir(libdir)):
        if not (f.endswith(".so") or ".so." in f):
            continue
        p = os.path.join(libdir, f)
        if os.path.getsize(p) < 4096:
            continue
        try:
            lib = Lib(p)
        except Exception:
            continue
        if lib.e.e_machine != 40:                                  # EM_ARM
            continue
        if want and f not in want:
            continue
        libs[f] = lib
    return libs


def provider(libs, sym):
    hits = [n for n, l in libs.items() if sym in l.exports]
    return hits[0] if hits else None


def reachable(libs, lname, sym, depth=3, limit=4000):
    """BFS：(库, 符号, 距离)。库外部符号记 (None, 符号, 距离)。"""
    out, seen = [], {(lname, sym)}
    out.append((lname, sym, 0))
    layer = [(lname, sym)]
    for d in range(1, depth + 1):
        nxt = []
        for ln, s in layer:
            lib = libs[ln]
            ent = lib.exports.get(s)
            if ent is None:
                continue
            res = lib.scan(ent, lib.span(ent))
            for c in res["calls"]:
                if c["sym"]:
                    pl = provider(libs, c["sym"]) if not c["inner"] else ln
                    key = (pl, c["sym"])
                    if key in seen:
                        continue
                    seen.add(key)
                    out.append((pl, c["sym"], d))
                    if pl:
                        nxt.append(key)
                else:
                    key = (None, "0x%x" % c["target"])
                    if key in seen:
                        continue
                    seen.add(key)
                    out.append((None, "0x%x" % c["target"], d))
            for tgt in res["callees"]:
                o = lib.owner(tgt)
                nm = lib.name_at.get(o) if o is not None else None
                key = (ln, nm or "0x%x" % tgt)
                if key in seen:
                    continue
                seen.add(key)
                out.append((ln, nm or "0x%x" % tgt, d))
                if nm:
                    nxt.append(key)
        layer = nxt
        if not layer or len(out) > limit:
            break
    return out


def db_views(lib, ent):
    """一个函数直接调 DB 接口时用的视图名/表名（r0 的实参）。"""
    res = lib.scan(ent, lib.span(ent))
    views = collections.Counter()
    for c in res["calls"]:
        if c["arg0"]:
            views[c["arg0"]] += 1
    return views


def cmd_tables(libs):
    rows = []
    for lname, lib in libs.items():
        for nm, va in sorted(lib.exports.items()):
            if not re.match(r"^Wlan\w*Db(Read|Write)$", nm):
                continue
            views = db_views(lib, va)
            main = [v for v in views if v in TABLES]
            rows.append({"func": nm, "lib": lname, "view": sorted(main),
                         "raw": sorted(views)})
    print("%-32s %-14s %-20s %s" % ("Db 函数", "所在库", "dbAPIGetView 的视图名", "备注"))
    print("-" * 100)
    for r in sorted(rows, key=lambda x: x["func"]):
        note = "（视图名由参数传入，需看调用方）" if not r["view"] else ""
        print("%-32s %-14s %-20s %s" % (r["func"], r["lib"],
                                        ",".join(r["view"]) or "-", note))
    return rows


def cmd_db_users(libs):
    """遍历所有"调用 DB 接口且 r0 是已知表名"的函数 —— 不靠命名规则。

    `--tables` 只认 `Wlan*Db*` 这个名字形态，会漏掉 mesh 决策面的 `Map*DB` 一族
    （`MapGetMasterDB → MultiAPMaster`、`MapGetMultiAPCfgDB → MultiAPCfg`）。
    这里反过来做：先挑出导入了 `dbAPIGetView`/`dbAPISetView` 的库，
    再把每个导出函数扫一遍，只看"调用那一刻 r0 是什么"。
    """
    rows = []
    for lname, lib in libs.items():
        imports = set(lib.plt.values())
        if not ({"dbAPIGetView", "dbAPISetView"} & imports):
            continue
        for nm, va in sorted(lib.exports.items()):
            res = lib.scan(va, lib.span(va))
            hit = collections.defaultdict(set)
            for c in res["calls"]:
                if c["arg0"] in TABLES:
                    hit[c["arg0"]].add("写" if c["sym"] == "dbAPISetView" else "读")
            for table, modes in hit.items():
                rows.append({"func": nm, "lib": lname, "table": table,
                             "mode": "".join(sorted(modes))})
    print("%-34s %-16s %-22s %s" % ("函数", "所在库", "表", "读/写"))
    print("-" * 90)
    for r in sorted(rows, key=lambda x: (x["table"], x["func"])):
        print("%-34s %-16s %-22s %s" % (r["func"], r["lib"], r["table"], r["mode"]))
    tables = sorted({r["table"] for r in rows})
    funcs = {(r["lib"], r["func"]) for r in rows}
    print("\n共 %d 个函数、%d 条「函数×表」记录、%d 张表：%s"
          % (len(funcs), len(rows), len(tables), ", ".join(tables)))
    return rows


def cmd_api(libs, apis, depth):
    result = {}
    for api in apis:
        where = provider(libs, api)
        if not where:
            print("! %s 不是本目录任何库的导出符号" % api)
            continue
        reach = reachable(libs, where, api, depth=depth)
        # 直接面（d<=1）是"这个 API 自己让谁去读写"；更深的是它调用的辅助函数顺手干的，
        # 混在一起会把规模算大（CmWLANSetWep 会因此看起来写了 8 张表）。
        direct = {"tables": set(), "views": set(), "db_funcs": set()}
        indirect = {"tables": set(), "db_funcs": set()}
        ext_hw, vendor_hw, ext = set(), set(), set()
        for lname, sym, d in reach:
            if lname is None:
                ext.add(sym)
                if any(k in sym.lower() for k in HW_WORDS):
                    ext_hw.add(sym)
                continue
            lib = libs[lname]
            ent = lib.exports.get(sym)
            names = list(db_views(lib, ent)) if ent is not None else []
            is_db = bool(re.match(r"^Wlan\w*Db(Read|Write)$", sym))
            bucket = direct if d <= 1 else indirect
            bucket["tables"].update(n for n in names if n in TABLES)
            if d <= 1:
                direct["views"].update(names)
            if is_db:
                bucket["db_funcs"].add(sym)
            if any(k in sym.lower() for k in VENDOR_WORDS):
                vendor_hw.add(sym)
        result[api] = {"lib": where,
                       "tables_direct": sorted(direct["tables"]),
                       "tables_reachable": sorted(indirect["tables"] - direct["tables"]),
                       "views": sorted(direct["views"]),
                       "db_direct": sorted(direct["db_funcs"]),
                       "db_indirect": sorted(indirect["db_funcs"] - direct["db_funcs"]),
                       "external_hw": sorted(ext_hw), "vendor": sorted(vendor_hw),
                       "symbols_reached": len(reach)}
        print("\n### %s  （%s，深度 %d，可达 %d 个符号）" % (api, where, depth, len(reach)))
        print("    表（直接） : %s" % (", ".join(sorted(direct["tables"])) or "—"))
        if indirect["tables"] - direct["tables"]:
            print("    表（间接） : %s" % ", ".join(sorted(indirect["tables"] - direct["tables"])))
        print("    Db（直接） : %s" % (", ".join(sorted(direct["db_funcs"])) or "—"))
        print("    视图名     : %s" % (", ".join(sorted(direct["views"])) or "-"))
        print("    ⚠ libc 系统调用 : %s" % (", ".join(sorted(ext_hw)) or "无"))
    return result


def main():
    ap = argparse.ArgumentParser(description="反推 Cm*/Wlan*Db* 与 DB 表的对应关系")
    ap.add_argument("libdir", help="原厂 .so 所在目录（kmodule/lib）")
    ap.add_argument("--tables", action="store_true", help="输出「Db 函数 → 表名」对照表")
    ap.add_argument("--db-users", action="store_true",
                    help="输出「谁读写哪张表」（不限命名，含 Map*DB 一族）")
    ap.add_argument("--api", nargs="*", default=[], help="逐个 API 展开落点")
    ap.add_argument("--multiapd-api", action="store_true",
                    help="展开 multiapd 实际导入的全部 Cm*/Cf*/Wlan*")
    ap.add_argument("--multiapd", help="multiapd 二进制路径（配合 --multiapd-api）")
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--json", help="把结果写成 JSON")
    args = ap.parse_args()

    if not os.path.isdir(args.libdir):
        sys.exit("不是目录：%s" % args.libdir)
    libs = load_dir(args.libdir)
    if not libs:
        sys.exit("目录里没有可用的 ARM32 共享库：%s" % args.libdir)
    print("载入 %d 个库" % len(libs))

    out = {}
    if args.tables:
        out["tables"] = cmd_tables(libs)
    if args.db_users:
        out["db_users"] = cmd_db_users(libs)

    apis = list(args.api)
    if args.multiapd_api:
        if not args.multiapd:
            sys.exit("--multiapd-api 需要 --multiapd <path>")
        m = Elf(args.multiapd)
        apis += sorted({s for s in m.plt_relocs().values()
                        if re.match(r"^(Cm|Cf|Wlan)", s)})
        apis = sorted({a for a in apis if provider(libs, a)})

    if apis:
        out["apis"] = cmd_api(libs, apis, args.depth)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=2, sort_keys=True)
        print("\n写入 %s（%d 个 API）" % (args.json, len(out.get("apis") or {})))


if __name__ == "__main__":
    main()
