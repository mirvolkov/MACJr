#!/usr/bin/env python3
"""Run a program built by cc on the host: a small Xtensa simulator.

It knows the instructions apps/cc/tinycc/xtensa-gen.c emits (24-bit
forms only, windowed CALLX8/ENTRY/RETW), loads the program ELF the way the
MACJr loader does (sections at their addresses, R_XTENSA_RELATIVE and
R_XTENSA_GLOB_DAT), and stands in for the firmware with Python versions of
the functions programs import (macjr_tty_write prints to stdout).

    xtsim.py program.elf

Exit status: main()'s return value.
"""
import struct
import sys

MEM_SIZE = 1 << 20
STACK_TOP = MEM_SIZE - 0x100
STUB_BASE = 0xF0000000
HEAP_BASE = 0x80000
RETURN_TO_HOST = 0xEEEEEEE0

B4CONST = [-1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256]
M32 = 0xFFFFFFFF


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Sim:
    def __init__(self, path):
        self.mem = bytearray(MEM_SIZE)
        self.heap = HEAP_BASE
        self.stubs = []
        self.out = []
        self.steps = 0
        self.load(path)

    # ---- memory
    def r8(self, a):
        return self.mem[a]

    def r16(self, a):
        return struct.unpack_from("<H", self.mem, a)[0]

    def r32(self, a):
        if a & 3:
            raise RuntimeError("unaligned 32-bit load at 0x%x" % a)
        return struct.unpack_from("<I", self.mem, a)[0]

    def w8(self, a, v):
        self.mem[a] = v & 0xFF

    def w16(self, a, v):
        struct.pack_into("<H", self.mem, a, v & 0xFFFF)

    def w32(self, a, v):
        if a & 3:
            raise RuntimeError("unaligned 32-bit store at 0x%x" % a)
        struct.pack_into("<I", self.mem, a, v & M32)

    def cstr(self, a):
        e = self.mem.index(0, a)
        return bytes(self.mem[a:e])

    # ---- the ELF, as the loader sees it
    def load(self, path):
        data = open(path, "rb").read()
        (e_entry,) = struct.unpack_from("<I", data, 24)
        e_shoff, = struct.unpack_from("<I", data, 32)
        e_shnum, e_shstrndx = struct.unpack_from("<HH", data, 48)
        shdrs = [struct.unpack_from("<10I", data, e_shoff + 40 * i) for i in range(e_shnum)]
        names_off = shdrs[e_shstrndx][4]

        def name(h):
            n = names_off + h[0]
            return data[n:data.index(b"\0", n)].decode()

        secs = {name(h): h for h in shdrs}
        for n in (".text", ".rodata", ".data"):
            if n in secs:
                h = secs[n]
                self.mem[h[3]:h[3] + h[5]] = data[h[4]:h[4] + h[5]]
        dynsym, dynstr = secs[".dynsym"], secs[".dynstr"]
        rela = secs[".rela.dyn"]
        for i in range(rela[5] // 12):
            off, info, _ = struct.unpack_from("<IIi", data, rela[4] + 12 * i)
            typ, symi = info & 0xFF, info >> 8
            if typ == 5:  # RELATIVE: the word already holds the address
                continue
            if typ != 3:
                raise RuntimeError("relocation type %d" % typ)
            st_name = struct.unpack_from("<I", data, dynsym[4] + 16 * symi)[0]
            n = dynstr[4] + st_name
            sym = data[n:data.index(b"\0", n)].decode()
            if not hasattr(self, "fw_" + sym):
                raise RuntimeError("no stand-in for firmware function %s" % sym)
            if sym not in self.stubs:
                self.stubs.append(sym)
            self.w32(off, STUB_BASE + 16 * self.stubs.index(sym))
        self.entry = e_entry

    # ---- running
    def run(self, args=(1, 0, 0)):
        regs = [0] * 16
        regs[1] = STACK_TOP
        for i, a in enumerate(args):
            regs[10 + i] = a
        self.frames = [regs]
        self.call(self.entry, RETURN_TO_HOST)
        pc = self.entry
        while True:
            pc = self.step(pc)
            if pc == RETURN_TO_HOST:
                return s32(self.frames[-1][10])

    def call(self, target, ret):
        caller = self.frames[-1]
        callee = [0] * 16
        callee[0] = ret
        callee[2:8] = caller[10:16]
        callee[1] = caller[1]  # until ENTRY moves it
        self.frames.append(callee)

    def retw(self):
        callee = self.frames.pop()
        self.frames[-1][10:16] = callee[2:8]
        return callee[0]

    def stub(self, target, ret_pc):
        name = self.stubs[(target - STUB_BASE) // 16]
        a = self.frames[-1]
        sp = a[1]
        words = a[10:16] + [self.r32(sp + 4 * i) for i in range(16)]
        r = getattr(self, "fw_" + name)(words)
        if r is None:
            r = 0
        a[10] = r & M32
        a[11] = (r >> 32) & M32
        return ret_pc

    def step(self, pc):
        self.steps += 1
        if self.steps > 50_000_000:
            raise RuntimeError("too many steps")
        a = self.frames[-1]
        w = self.mem[pc] | self.mem[pc + 1] << 8 | self.mem[pc + 2] << 16
        op0 = w & 15
        t = (w >> 4) & 15
        s = (w >> 8) & 15
        r = (w >> 12) & 15
        op1 = (w >> 16) & 15
        op2 = (w >> 20) & 15
        nxt = pc + 3

        def bad():
            raise RuntimeError("unknown instruction %06x at 0x%x" % (w, pc))

        if op0 == 0:
            if op1 == 0:
                if op2 == 0:
                    if r == 0 and t == 0xE:  # callx8
                        target = a[s]
                        if target >= STUB_BASE:
                            return self.stub(target, nxt)
                        self.call(target, nxt)
                        return target
                    if r == 0 and t == 0xA:  # jx
                        return a[s]
                    if w == 0x90:  # retw
                        return self.retw()
                    if w == 0x20F0:  # nop
                        return nxt
                    bad()
                elif op2 == 1:
                    a[r] = a[s] & a[t]
                elif op2 == 2:
                    a[r] = a[s] | a[t]
                elif op2 == 3:
                    a[r] = a[s] ^ a[t]
                elif op2 == 4:
                    if r == 1:  # ssl
                        self.sar = 32 - (a[s] & 31)
                    elif r == 0:  # ssr
                        self.sar = a[s] & 31
                    else:
                        bad()
                elif op2 == 6 and s == 0:
                    a[r] = (-a[t]) & M32
                elif op2 == 8:
                    a[r] = (a[s] + a[t]) & M32
                elif op2 == 0xC:
                    a[r] = (a[s] - a[t]) & M32
                else:
                    bad()
            elif op1 == 1:
                if op2 in (0, 1):
                    sa = 32 - ((op2 & 1) << 4 | t)
                    a[r] = (a[s] << sa) & M32
                elif op2 in (2, 3):
                    sa = (op2 & 1) << 4 | s
                    a[r] = (s32(a[t]) >> sa) & M32
                elif op2 == 4:
                    a[r] = a[t] >> s
                elif op2 == 0xA:  # sll
                    a[r] = (a[s] << (32 - self.sar)) & M32 if self.sar != 32 else a[s]
                elif op2 == 9:
                    a[r] = a[t] >> self.sar
                elif op2 == 0xB:
                    a[r] = (s32(a[t]) >> self.sar) & M32
                else:
                    bad()
            elif op1 == 2:
                x, y = a[s], a[t]
                if op2 == 8:
                    a[r] = (x * y) & M32
                elif op2 == 0xA:
                    a[r] = (x * y) >> 32
                elif op2 in (0xC, 0xD, 0xE, 0xF):
                    if y == 0:
                        raise RuntimeError("divide by zero at 0x%x" % pc)
                    if op2 == 0xC:
                        a[r] = x // y
                    elif op2 == 0xE:
                        a[r] = x % y
                    else:
                        sx, sy = s32(x), s32(y)
                        q = abs(sx) // abs(sy) * (1 if (sx < 0) == (sy < 0) else -1)
                        a[r] = (q if op2 == 0xD else sx - q * sy) & M32
                else:
                    bad()
            elif op1 == 3 and op2 == 2:  # sext
                a[r] = sext(a[s], t + 8) & M32
            elif op1 in (4, 5):  # extui
                sh = (op1 & 1) << 4 | s
                a[r] = (a[t] >> sh) & ((1 << (op2 + 1)) - 1)
            else:
                bad()
            return nxt
        if op0 == 1:  # l32r
            imm16 = (w >> 8) & 0xFFFF
            addr = ((pc + 3) & ~3) + ((imm16 - 0x10000) << 2)
            a[t] = self.r32(addr)
            return nxt
        if op0 == 2:
            imm8 = (w >> 16) & 0xFF
            base = a[s]
            if r == 0:
                a[t] = self.r8(base + imm8)
            elif r == 1:
                a[t] = self.r16(base + 2 * imm8)
            elif r == 9:
                a[t] = sext(self.r16(base + 2 * imm8), 16) & M32
            elif r == 2:
                a[t] = self.r32(base + 4 * imm8)
            elif r == 4:
                self.w8(base + imm8, a[t])
            elif r == 5:
                self.w16(base + 2 * imm8, a[t])
            elif r == 6:
                self.w32(base + 4 * imm8, a[t])
            elif r == 0xA:
                a[t] = sext(s << 8 | imm8, 12) & M32
            elif r == 0xC:
                a[t] = (base + sext(imm8, 8)) & M32
            elif r == 0xD:
                a[t] = (base + (sext(imm8, 8) << 8)) & M32
            else:
                bad()
            return nxt
        if op0 == 6:
            n = t & 3
            m = t >> 2
            if n == 0:  # j
                return pc + 4 + sext(w >> 6, 18)
            if n == 1:  # bz
                x = s32(a[s])
                taken = [x == 0, x != 0, x < 0, x >= 0][m]
                return pc + 4 + sext(w >> 12, 12) if taken else nxt
            if n == 2:  # bi
                x, c = s32(a[s]), B4CONST[r]
                taken = [x == c, x != c, x < c, x >= c][m]
                return pc + 4 + sext(w >> 16, 8) if taken else nxt
            if n == 3 and m == 0:  # entry
                frame = ((w >> 12) & 0xFFF) * 8
                a[1] = (a[1] - frame) & M32
                return nxt
            bad()
        if op0 == 7:
            x, y = a[s], a[t]
            conds = {1: x == y, 9: x != y, 2: s32(x) < s32(y), 0xA: s32(x) >= s32(y),
                     3: x < y, 0xB: x >= y}
            if r not in conds:
                bad()
            return pc + 4 + sext(w >> 16, 8) if conds[r] else nxt
        bad()

    # ---- the firmware, in Python
    def fw_macjr_tty_write(self, w):
        text = bytes(self.mem[w[1]:w[1] + w[2]])
        self.out.append(text)
        sys.stdout.write(text.decode("cp866", "replace"))
        return w[2]

    def fw_macjr_tty_header(self, w):
        sys.stdout.write("\n==[%s]==\n" % self.cstr(w[1]).decode())

    def fw_macjr_display_flush(self, w):
        sys.stdout.flush()

    def fw_sys_getkey(self, w):
        return 27

    def fw_sys_millis(self, w):
        return self.steps // 240

    def fw_strlen(self, w):
        return len(self.cstr(w[0]))

    def fw_strcmp(self, w):
        x, y = self.cstr(w[0]), self.cstr(w[1])
        return (x > y) - (x < y)

    def fw_strcpy(self, w):
        src = self.cstr(w[1]) + b"\0"
        self.mem[w[0]:w[0] + len(src)] = src
        return w[0]

    def fw_strcat(self, w):
        end = w[0] + len(self.cstr(w[0]))
        src = self.cstr(w[1]) + b"\0"
        self.mem[end:end + len(src)] = src
        return w[0]

    def fw_memmove(self, w):
        self.mem[w[0]:w[0] + w[2]] = bytes(self.mem[w[1]:w[1] + w[2]])
        return w[0]

    fw_memcpy = fw_memmove

    def fw_memset(self, w):
        self.mem[w[0]:w[0] + w[2]] = bytes([w[1] & 0xFF]) * w[2]
        return w[0]

    def fw_malloc(self, w):
        p = self.heap
        self.heap = (self.heap + w[0] + 15) & ~15
        return p

    def fw_free(self, w):
        pass

    @staticmethod
    def ll(w, i):
        return w[i] | w[i + 1] << 32

    def fw___divdi3(self, w):
        x, y = sext(self.ll(w, 0), 64), sext(self.ll(w, 2), 64)
        return abs(x) // abs(y) * (1 if (x < 0) == (y < 0) else -1)

    def fw___moddi3(self, w):
        x, y = sext(self.ll(w, 0), 64), sext(self.ll(w, 2), 64)
        return x - self.fw___divdi3(w) * y

    def fw___udivdi3(self, w):
        return self.ll(w, 0) // self.ll(w, 2)

    def fw___umoddi3(self, w):
        return self.ll(w, 0) % self.ll(w, 2)

    def fw___ashldi3(self, w):
        return (self.ll(w, 0) << w[2]) & ((1 << 64) - 1)

    def fw___ashrdi3(self, w):
        return sext(self.ll(w, 0), 64) >> w[2]

    def fw___lshrdi3(self, w):
        return self.ll(w, 0) >> w[2]

    def fw_snprintf(self, w):
        buf, size, fmt = w[0], w[1], self.cstr(w[2]).decode("latin-1")
        i, out, k = 3, "", 0
        while k < len(fmt):
            c = fmt[k]
            k += 1
            if c != "%":
                out += c
                continue
            spec = ""
            while fmt[k] in "-0123456789l":
                spec += fmt[k]
                k += 1
            conv = fmt[k]
            k += 1
            if conv == "%":
                out += "%"
                continue
            if "ll" in spec:
                i += i & 1  # 64-bit varargs start on an even word
                v = self.ll(w, i)
                i += 2
                if conv == "d":
                    v = sext(v, 64)
            else:
                v = w[i]
                i += 1
                if conv == "d":
                    v = s32(v)
            pyspec = spec.replace("l", "")
            if conv == "s":
                out += ("%" + pyspec + "s") % self.cstr(v).decode("latin-1")
            elif conv == "c":
                out += chr(v & 0xFF)
            elif conv in "dux":
                out += ("%" + pyspec + ("x" if conv == "x" else "d")) % v
            else:
                raise RuntimeError("snprintf: %%%s" % conv)
        b = out.encode("latin-1")[:max(size - 1, 0)] + b"\0"
        self.mem[buf:buf + len(b)] = b
        return len(out)


def main():
    sim = Sim(sys.argv[1])
    try:
        ret = sim.run()
    except RuntimeError as e:
        print("\nxtsim: %s" % e)
        return 125
    print("\nxtsim: main() returned %d after %d instructions" % (ret, sim.steps))
    return ret & 0xFF


if __name__ == "__main__":
    sys.exit(main())
