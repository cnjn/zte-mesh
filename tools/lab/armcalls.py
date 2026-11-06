"""谁调用了 system()/popen()：ARM32 静态调用点定位，零依赖（不需 binutils / capstone）。

为什么需要它：判断"某厂商进程是否真的走 shell 调外部工具"（例如 cspd 是否真的执行
`iwpriv %s set K=V`），动态验证要改只读的 squashfs 或重启守护进程，代价高且有风险；
静态定位调用点 + 回溯 r0 指向的字符串，结论一样硬，且完全离线。

原理（ARM32 ELF，动态链接）：
  1. `.rel.plt` 给出每个被导入符号的 GOT 槽地址（r_offset）。
  2. ARM 的 PLT stub 末尾一定是 `ldr pc, [pc, #-4]`，紧跟 4 字节字面量 = 那个 GOT 槽地址；
     所以在文件里搜"GOT 槽地址这 4 个字节"，命中处前 12 字节就是 stub 入口。
  3. 扫 `.text` 里所有 `bl`（0xEBxxxxxx），目标是 stub 入口的就是一次调用。
  4. 从 `bl` 往前若干条指令找 `ldr r0,[pc,#±imm]` / `adr r0` / `mov r0,#imm`，
     解析出它加载的地址，再读该地址处的字符串。
     回溯失败（r0 来自寄存器/栈/其他函数的返回值）就如实标 `<非字面量>` ——
     这正是 fopen 类动态拼串信号，不要硬猜。

用法:
    python3 tools/lab/armcalls.py <elf> system popen
    python3 tools/lab/armcalls.py <elf> system popen --back 60 --all
"""

import struct
import sys

PT_LOAD = 1
SHT_DYNSYM = 11
DT_PLTRELSZ, DT_PLTREL, DT_JMPREL, DT_SYMTAB, DT_STRTAB = 2, 20, 23, 6, 5


# ---------------------------------------------------------------- ELF 骨架

class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.d = d = f.read()
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise ValueError("只支持 ELF32 小端")
        self.e_type = struct.unpack_from("<H", d, 16)[0]
        self.e_machine = struct.unpack_from("<H", d, 18)[0]
        self.phoff = struct.unpack_from("<I", d, 28)[0]
        self.shoff = struct.unpack_from("<I", d, 32)[0]
        self.phentsize, self.phnum = struct.unpack_from("<HH", d, 42)
        self.shentsize, self.shnum, self.shstrndx = struct.unpack_from("<HHH", d, 46)

        self.loads = []          # (vaddr, filesz, offset)
        for i in range(self.phnum):
            o = self.phoff + i * self.phentsize
            p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIIII", d, o)
            if p_type == PT_LOAD and p_filesz:
                self.loads.append((p_vaddr, p_filesz, p_offset))
        self.loads.sort()

        self.sections = {}
        self.sec_by_name = {}
        if self.shnum:
            strtab_off = None
            if self.shstrndx < self.shnum:
                strtab_off = struct.unpack_from(
                    "<I", d, self.shoff + self.shstrndx * self.shentsize + 16)[0]
            for i in range(self.shnum):
                o = self.shoff + i * self.shentsize
                name, sh_type, _flags, addr, off, size = struct.unpack_from("<IIIIII", d, o)
                nm = ""
                if strtab_off is not None:
                    end = d.find(b"\x00", strtab_off + name)
                    nm = d[strtab_off + name:end].decode("latin-1")
                self.sections[i] = (name, sh_type, addr, off, size)
                self.sec_by_name.setdefault(nm, (addr, off, size))

    def section(self, name):
        """(addr, offset, size)，无此段返回 None。"""
        return self.sec_by_name.get(name)

    def va2off(self, va):
        for v, sz, off in self.loads:
            if v <= va < v + sz:
                return off + (va - v)
        return None

    def off2va(self, off):
        for v, sz, o in self.loads:
            if o <= off < o + sz:
                return v + (off - o)
        return None

    def read_va(self, va, n):
        off = self.va2off(va)
        if off is None:
            return b""
        return self.d[off:off + n]

    def cstr(self, va, maxlen=160):
        off = self.va2off(va)
        if off is None:
            return ""
        end = self.d.find(b"\x00", off, off + maxlen)
        if end < 0:
            end = off + maxlen
        raw = self.d[off:end]
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            return raw.decode("latin-1")

    # ------------------------------------------------------------ 动态段

    def dynamic(self):
        out = {}
        for _, sh_type, addr, off, size in self.sections.values():
            if sh_type != 6:      # SHT_DYNAMIC
                continue
            for i in range(size // 8):
                tag, val = struct.unpack_from("<iI", self.d, off + i * 8)
                if tag == 0:
                    break
                out.setdefault(tag, val)
        return out

    def dynsym(self):
        """返回 [(name, value)]，索引即 dynsym 下标。"""
        out = []
        for _, sh_type, addr, off, size in self.sections.values():
            if sh_type != SHT_DYNSYM:
                continue
            dyn = self.dynamic()
            stroff = self.va2off(dyn.get(DT_STRTAB, 0))
            for i in range(size // 16):
                st_name = struct.unpack_from("<I", self.d, off + i * 16)[0]
                st_value = struct.unpack_from("<I", self.d, off + i * 16 + 4)[0]
                nm = ""
                if stroff is not None:
                    end = self.d.find(b"\x00", stroff + st_name)
                    nm = self.d[stroff + st_name:end].decode("latin-1")
                out.append((nm, st_value))
        return out

    def plt_relocs(self):
        """{GOT槽VA: 符号名}"""
        dyn = self.dynamic()
        jmprel = dyn.get(DT_JMPREL)
        pltsz = dyn.get(DT_PLTRELSZ, 0)
        syms = self.dynsym()
        out = {}
        if not jmprel:
            return out
        off = self.va2off(jmprel)
        if off is None:
            return out
        for i in range(pltsz // 8):      # Elf32_Rel，无 addend
            r_offset, r_info = struct.unpack_from("<II", self.d, off + i * 8)
            idx = r_info >> 8
            nm = syms[idx][0] if idx < len(syms) else "?"
            out[r_offset] = nm
        return out


# ---------------------------------------------------------------- 指令解析

def sext(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


def arm_rot_imm(v):
    """ARM 立即数：8 位值 + 4 位右旋位数×2"""
    imm8 = v & 0xFF
    rot = ((v >> 8) & 0xF) * 2
    return ((imm8 >> rot) | (imm8 << (32 - rot))) & 0xFFFFFFFF if rot else imm8


def find_plt_stubs(elf, got2sym):
    """{stub入口VA: 符号名}。

    这里的 PLT 是 GNU ARM 的**经典 lazy 版式**，每个 stub 12 字节、没有字面量：
        e28fc602   add ip, pc, #<hi>              ; ip = pc+8+hi
        e28cca31   add ip, ip, #<mid>             ; ip += mid   → 页内基址
        e5bcfb84   ldr pc, [ip, #<slot-ip>]!      ; 跳到 GOT 槽
    所以不能靠"搜 GOT 槽字节"来找（那只会命中 .rel.plt 重定位表本身），也不能用
    `imm12 == slot & 0xFFF` 校验——imm12 是相对那个**非页对齐**基址的偏移。
    正确做法：把三条指令的立即数累加出真实 GOT 地址，再回查重定位表，与顺序无关。
    """
    plt = elf.section(".plt")
    if not plt or not got2sym:
        return {}
    plt_va, plt_size = plt[0], plt[2]
    stubs = {}
    bad = 0
    k = 0
    while True:
        va = plt_va + 20 + 12 * k          # resolver 占 20 字节（GNU ARM 固定）
        if va + 12 > plt_va + plt_size:
            break
        w0, w1, w2 = struct.unpack("<III", elf.read_va(va, 12))
        k += 1
        if ((w0 & 0xFFFFF000) != 0xE28FC000 or      # add ip, pc, #imm
                (w1 & 0xFFFFF000) != 0xE28CC000 or  # add ip, ip, #imm
                (w2 & 0xFFFFF000) != 0xE5BCF000):   # ldr pc, [ip, #imm]!
            bad += 1
            continue
        ip = va + 8 + arm_rot_imm(w0 & 0xFFF)
        ip += arm_rot_imm(w1 & 0xFFF)
        slot = ip + (w2 & 0xFFF)
        if slot in got2sym:
            stubs[va] = got2sym[slot]
    if bad > 8:
        print("  ⚠ PLT 版式异常 %d 个 stub 不匹配经典形态" % bad)
    return stubs


def backward_r0(elf, bl_va, steps):
    """从 bl 往前找 r0 的来源，返回 (字符串, 说明)。"""
    off = elf.va2off(bl_va)
    if off is None:
        return None, "?"
    for k in range(1, steps + 1):
        p = off - 4 * k
        if p < 0:
            break
        w = struct.unpack_from("<I", elf.d, p)[0]
        va = bl_va - 4 * k
        # ldr r0, [pc, #±imm]
        if (w & 0x0F7F0000) == 0x051F0000 and ((w >> 12) & 0xF) == 0:
            u = (w >> 23) & 1
            imm = w & 0xFFF
            lit_va = va + 8 + (imm if u else -imm)
            lit = elf.read_va(lit_va, 4)
            if len(lit) == 4:
                target = struct.unpack("<I", lit)[0]
                s = elf.cstr(target)
                if s:
                    return s, "@0x%x(ldr)" % va
                return None, "ldr→0x%x(非字符串)" % target
        # adr r0 (add/sub r0, pc, #imm)
        if (w & 0x0FFF0000) in (0x028F0000, 0x024F0000):
            imm = arm_rot_imm(w & 0xFFF)
            target = (va + 8 + imm) if (w & 0x0200000) else (va + 8 - imm)
            target &= 0xFFFFFFFF
            s = elf.cstr(target)
            if s:
                return s, "@0x%x(adr)" % va
            return None, "adr→0x%x(非字符串)" % target
        # mov r0, #imm
        if (w & 0x0FEF0000) == 0x03A00000:
            return None, "@0x%x mov r0,#%d（整数参数）" % (va, arm_rot_imm(w & 0xFFF))
        # 遇到写 r0 的其他形式（pop {r0,...} / mov r0, rx）就停
        if ((w >> 12) & 0xF) == 0 and (w & 0x0C000000) in (0x00000000, 0x08000000):
            if (w & 0x0FE00000) in (0x00800000, 0x01A00000, 0x03A00000, 0x04100000):
                return None, "@0x%x 其它写 r0 形式" % va
    return None, "<回溯窗内未见字面量>"


def scan(elf, targets, back, show_all):
    got2sym = elf.plt_relocs()
    print("★ %s：%d 个 PLT 重定位" % (elf.path, len(got2sym)))
    want = {name: slot for slot, name in got2sym.items() if name in targets}
    stubs = find_plt_stubs(elf, got2sym)          # 必须传全表，GOT 地址才查得到
    stub2name = dict(stubs)
    print("★ 定位到 PLT stub %d 个（自校验通过）" % len(stub2name))
    for t in targets:
        if t not in want:
            print("  ! %s 不是动态导入符号" % t)
        elif not any(v == t for v in stub2name.values()):
            print("  ! %s 有重定位但没找到对应 stub" % t)

    # 扫全部可执行段里的 bl
    import collections
    found = collections.defaultdict(list)
    for v, sz, off in elf.loads:
        p = off
        end = off + sz - 4
        while p <= end:
            w = struct.unpack_from("<I", elf.d, p)[0]
            if (w & 0x0F000000) == 0x0B000000:
                va = v + (p - off)
                tgt = va + 8 + (sext(w & 0xFFFFFF, 24) << 2)
                if tgt in stub2name:
                    found[stub2name[tgt]].append(va)
            p += 4
    hits = found

    for name in targets:
        lst = hits.get(name, [])
        print("\n=== %s：%d 处调用 ===" % (name, len(lst)))
        shown = 0
        for va in lst:
            s, how = backward_r0(elf, va, back)
            if not show_all and s is None and shown > 2:
                continue
            shown += 1
            print("  0x%08x  %s" % (va, how))
            if s:
                print("            arg: |%s|" % s[:150])
        if not show_all and len(lst) > shown:
            print("  …（另有 %d 处 r0 非字面量/未回溯，--all 看全）" % (len(lst) - shown))
    return hits


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path = argv[1]
    rest = argv[2:]
    back, show_all = 40, False
    if "--back" in rest:
        i = rest.index("--back")
        back = int(rest[i + 1])
        del rest[i:i + 2]
    if "--all" in rest:
        rest.remove("--all")
        show_all = True
    elf = Elf(path)
    scan(elf, rest, back, show_all)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
