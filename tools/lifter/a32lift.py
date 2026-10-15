"""Static recompiler: AArch32 (A32 encoding) -> C, for the 32-bit modules of MK8DX.

Modelled on ssbu-recomp's tools/lifter/recomp.py (AArch64), GPLv3: every guest function becomes
`void f_<addr>(Ctx *c)` working on a register context (runtime/a32rt.h); guest memory is reserved at
GUEST_BASE, so guest address a is the host pointer GUEST_BASE + a.

Function boundaries and code/data come from IDA (tools/ida_export.py): ARM keeps literal pools between
code, so a linear sweep is not enough. A function is lifted with all of its chunks; any branch to an address
inside the same function is a goto, a branch elsewhere is a tail call.

Usage:
    python a32lift.py cover <module.elf> <ida.json>                 coverage statistics, nothing written
    python a32lift.py lift  <module.elf> <ida.json> <outdir> [--base 0xADDR] [--only 0xEA,...]
"""
import bisect
import collections
import json
import os
import struct
import sys

OFF = 0x1000                     # file offset of guest address 0 in the ELF written by nso2elf.py


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xffffffff if n else v


class Unimpl(Exception):
    pass


def u32(v):
    return '0x%xu' % (v & 0xffffffff)


def f32_bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


# ----------------------------------------------------------------------------- module
class Module:
    def __init__(self, path, base=0):
        self.img = open(path, 'rb').read()
        assert self.img[4] == 1, 'expected a 32-bit ELF from nso2elf.py'
        self.base = base
        mod = struct.unpack_from('<I', self.img, OFF + 4)[0]
        dyn = mod + struct.unpack_from('<i', self.img, OFF + mod + 4)[0]
        self.tags = {}
        n = 0
        while True:
            t, v = struct.unpack_from('<iI', self.img, OFF + dyn + n * 8)
            n += 1
            if t == 0:
                break
            self.tags.setdefault(t, v)
        symtab, strtab = self.tags[6], self.tags[5]
        nsym = struct.unpack_from('<I', self.img, OFF + self.tags[4] + 4)[0]
        self.syms = []
        for i in range(nsym):
            name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.img, OFF + symtab + i * 16)
            end = self.img.index(b'\0', OFF + strtab + name)
            self.syms.append((self.img[OFF + strtab + name:end].decode('ascii', 'replace'), value, info, shndx))
        # relocations: GOT slot -> symbol index for JUMP_SLOT; code pointers from RELATIVE/ABS32
        self.jump_slots = {}
        self.code_ptrs = set()
        for t_off, t_sz in ((17, 18), (23, 2)):
            if t_off not in self.tags:
                continue
            off, sz = self.tags[t_off], self.tags[t_sz]
            for i in range(sz // 8):
                r_off, info = struct.unpack_from('<II', self.img, OFF + off + i * 8)
                typ, sym = info & 0xff, info >> 8
                if typ == 22:
                    self.jump_slots[r_off] = sym
                elif typ == 23:
                    self.code_ptrs.add(self.word(r_off))
                elif typ == 2 and sym and self.syms[sym][3]:
                    self.code_ptrs.add(self.syms[sym][1] + self.word(r_off))

    def word(self, a):
        return struct.unpack_from('<I', self.img, OFF + a)[0]

    def plt_target(self, a):
        """If a is an lld ARM PLT stub (add ip, pc, #x; add ip, ip, #y; ldr pc, [ip, #z]!), return its symbol index."""
        try:
            w0, w1, w2 = self.word(a), self.word(a + 4), self.word(a + 8)
        except struct.error:
            return None
        if (w0 & 0xfffff000) != 0xe28fc000 or (w1 & 0xfffff000) != 0xe28cc000 or (w2 & 0xfffff000) != 0xe5bcf000:
            return None
        slot = a + 8 + ror(bits(w0, 7, 0), 2 * bits(w0, 11, 8)) + ror(bits(w1, 7, 0), 2 * bits(w1, 11, 8)) + bits(w2, 11, 0)
        return self.jump_slots.get(slot)


def mangle(name):
    return ''.join(ch if ch.isalnum() else '_' for ch in name)


# ----------------------------------------------------------------------------- operands
def R(n, pc):
    """Read register n; PC reads as the instruction address + 8."""
    return u32(pc + 8) if n == 15 else 'c->r[%d]' % n


def shift_imm(expr, t, n):
    """(value, carry) C expressions of an immediate shift of expr; carry None = unchanged."""
    if t == 0:
        if n == 0:
            return expr, None
        return '(%s << %d)' % (expr, n), '((%s >> %d) & 1)' % (expr, 32 - n)
    if t == 1:
        if n == 0:
            return '0u', '(%s >> 31)' % expr
        return '(%s >> %d)' % (expr, n), '((%s >> %d) & 1)' % (expr, n - 1)
    if t == 2:
        if n == 0:
            return '(uint32_t)((int32_t)%s >> 31)' % expr, '(%s >> 31)' % expr
        return '(uint32_t)((int32_t)%s >> %d)' % (expr, n), '((%s >> %d) & 1)' % (expr, n - 1)
    if n == 0:                                                    # RRX
        return '((c->C << 31) | (%s >> 1))' % expr, '(%s & 1)' % expr
    return 'ror32(%s, %d)' % (expr, n), '((%s >> %d) & 1)' % (expr, n - 1)


DP_NAMES = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc', 'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn']


# ----------------------------------------------------------------------------- lifter
class Fn:
    """Lifting context for one guest function: its instruction addresses (all chunks)."""

    def __init__(self, env, entry, addrs):
        self.env, self.entry, self.addrs = env, entry, addrs
        self.has_dispatch = False

    # ---- control flow helpers
    def goto(self, target):
        if target in self.addrs:
            return 'goto L_%x;' % target
        return '{ %s return; }' % self.env.call(target)

    def write_pc(self, expr, is_return=False):
        """A data-processing or load result goes to the PC."""
        if is_return:
            return 'return;'
        self.has_dispatch = True
        return '{ jt = %s; goto L_dispatch; }' % expr

    # ---- top level
    def lift(self, w, pc):
        cc = w >> 28
        if cc == 0xf:
            return self.uncond(w, pc)
        s = self.body(w, pc)
        if cc == 0xe:
            return s
        return 'if (cond(c, %d)) { %s }' % (cc, s)

    def body(self, w, pc):
        op = bits(w, 27, 25)
        if op == 0:
            if bits(w, 7, 4) == 0x9:
                return self.multiply_or_sync(w, pc)
            if bits(w, 7, 7) and bits(w, 4, 4):
                return self.extra_ls(w, pc)
            if bits(w, 24, 23) == 2 and not bits(w, 20, 20):
                return self.misc(w, pc)
            return self.dp(w, pc, imm=False)
        if op == 1:
            if bits(w, 24, 23) == 2 and not bits(w, 20, 20):
                return self.dp_imm_special(w, pc)
            return self.dp(w, pc, imm=True)
        if op == 2:
            return self.ls_word(w, pc, reg=False)
        if op == 3:
            if bits(w, 4, 4):
                return self.media(w, pc)
            return self.ls_word(w, pc, reg=True)
        if op == 4:
            return self.ldm_stm(w, pc)
        if op == 5:
            target = (pc + 8 + (sext(bits(w, 23, 0), 24) << 2)) & 0xffffffff
            if bits(w, 24, 24):
                return 'c->r[14] = %s; %s' % (u32(pc + 4), self.env.call(target))
            return self.goto(target)
        if op == 6:
            return self.coproc_ls(w, pc)
        if bits(w, 24, 24):
            return 'guest_svc(c, 0x%x);' % bits(w, 23, 0)
        return self.coproc(w, pc)

    # ---- data processing
    def operand2(self, w, pc, imm, need_carry):
        """(value expr, carry expr or None, setup statements)."""
        if imm:
            rot = bits(w, 11, 8) * 2
            v = ror(bits(w, 7, 0), rot)
            return u32(v), (str(v >> 31) if rot else None), ''
        rm, t = bits(w, 3, 0), bits(w, 6, 5)
        if bits(w, 4, 4):                                           # register-shifted register
            if rm == 15 or bits(w, 11, 8) == 15:
                raise Unimpl('dp-rsr-pc')
            setup = 'uint32_t op2 = shreg(c, c->r[%d], %d, c->r[%d], %d);' % (rm, t, bits(w, 11, 8), 1 if need_carry else 0)
            return 'op2', None, setup
        v, cy = shift_imm(R(rm, pc), t, bits(w, 11, 7))
        return v, cy, ''

    def dp(self, w, pc, imm):
        opc, s, rn, rd = bits(w, 24, 21), bits(w, 20, 20), bits(w, 19, 16), bits(w, 15, 12)
        logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
        op2, cy, setup = self.operand2(w, pc, imm, s and logical)
        a = R(rn, pc)
        if opc in (8, 9, 10, 11) and not s:
            raise Unimpl('dp-test-nos')
        if rd == 15 and s:
            raise Unimpl('dp-pc-s')                                 # exception return
        if not s:
            e = {0: '%s & %s', 1: '%s ^ %s', 2: '%s - %s', 3: '%s - %s', 4: '%s + %s', 5: '%s + %s + c->C',
                 6: '%s - %s - !c->C', 7: '%s - %s - !c->C', 12: '%s | %s', 14: '%s & ~%s'}
            if opc == 13:
                expr = op2
            elif opc == 15:
                expr = '~%s' % op2
            elif opc in (3, 7):
                expr = e[opc] % (op2, a)
            else:
                expr = e[opc] % (a, op2)
            if rd == 15:
                ret = opc == 13 and not imm and bits(w, 3, 0) == 14 and bits(w, 11, 4) == 0
                return '{ %s %s }' % (setup, self.write_pc(expr, is_return=ret))
            return '{ %s c->r[%d] = %s; }' % (setup, rd, expr) if setup else 'c->r[%d] = %s;' % (rd, expr)
        # flag-setting
        if logical:
            expr = {0: '%s & op2', 1: '%s ^ op2', 8: '%s & op2', 9: '%s ^ op2', 12: '%s | op2', 13: 'op2',
                    14: '%s & ~op2', 15: '~op2'}[opc]
            if '%s' in expr:
                expr = expr % a
            pre = setup if setup else 'uint32_t op2 = %s;' % op2
            carry = ''
            if cy is not None and not setup:                     # shifter carry, taken before Rd is written
                pre += ' uint32_t cy = %s;' % cy
                carry = ' c->C = cy;'
            res = 'setnz(c, %s)' % expr
            if opc in (8, 9):
                return '{ %s (void)%s;%s }' % (pre, res, carry)
            return '{ %s uint32_t res = %s;%s c->r[%d] = res; }' % (pre, res, carry, rd)
        arith = {2: 'adds32(c, %s, ~op2, 1)', 3: 'adds32(c, op2, ~%s, 1)', 4: 'adds32(c, %s, op2, 0)',
                 5: 'adds32(c, %s, op2, c->C)', 6: 'adds32(c, %s, ~op2, c->C)', 7: 'adds32(c, op2, ~%s, c->C)',
                 10: 'adds32(c, %s, ~op2, 1)', 11: 'adds32(c, %s, op2, 0)'}[opc] % a
        pre = setup if setup else 'uint32_t op2 = %s;' % op2
        if opc in (10, 11):
            return '{ %s (void)%s; }' % (pre, arith)
        return '{ %s c->r[%d] = %s; }' % (pre, rd, arith)

    def dp_imm_special(self, w, pc):
        op = bits(w, 22, 21)
        rd = bits(w, 15, 12)
        if op == 0:                                                 # MOVW
            return 'c->r[%d] = %s;' % (rd, u32((bits(w, 19, 16) << 12) | bits(w, 11, 0)))
        if op == 2:                                                 # MOVT
            return 'c->r[%d] = (c->r[%d] & 0xffffu) | %s;' % (rd, rd, u32(((bits(w, 19, 16) << 12) | bits(w, 11, 0)) << 16))
        if op == 1 and bits(w, 19, 16) == 0:                        # hints: NOP, YIELD, WFE, WFI, SEV
            return ';'
        if op == 1 and bits(w, 19, 18):                             # MSR APSR, #imm
            v = ror(bits(w, 7, 0), bits(w, 11, 8) * 2)
            s = ''
            if bits(w, 19, 19):
                s += 'c->N = %d; c->Z = %d; c->C = %d; c->V = %d; c->Q = %d; ' % (v >> 31, (v >> 30) & 1, (v >> 29) & 1, (v >> 28) & 1, (v >> 27) & 1)
            if bits(w, 18, 18):
                s += 'c->GE = %d; ' % ((v >> 16) & 0xf)
            return s
        raise Unimpl('dp-imm-special')

    # ---- multiply and synchronisation
    def multiply_or_sync(self, w, pc):
        if bits(w, 24, 24) == 0:
            op, s = bits(w, 23, 21), bits(w, 20, 20)
            rd, ra, rm, rn = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
            if op == 0:
                e = 'c->r[%d] * c->r[%d]' % (rn, rm)
            elif op == 1:
                e = 'c->r[%d] * c->r[%d] + c->r[%d]' % (rn, rm, ra)
            elif op == 3:
                e = 'c->r[%d] - c->r[%d] * c->r[%d]' % (ra, rn, rm)
            else:
                lo, hi = ra, rd
                signed = op in (6, 7)
                prod = '(uint64_t)((int64_t)(int32_t)c->r[%d] * (int32_t)c->r[%d])' % (rn, rm) if signed else \
                       '((uint64_t)c->r[%d] * c->r[%d])' % (rn, rm)
                if op in (4, 6):
                    acc = ''
                elif op in (5, 7):
                    acc = ' + (((uint64_t)c->r[%d] << 32) | c->r[%d])' % (hi, lo)
                else:                                               # UMAAL
                    acc = ' + c->r[%d] + c->r[%d]' % (hi, lo)
                fl = ' c->N = (uint32_t)(p >> 63); c->Z = p == 0;' if s else ''
                return '{ uint64_t p = %s%s; c->r[%d] = (uint32_t)p; c->r[%d] = (uint32_t)(p >> 32);%s }' % (prod, acc, lo, hi, fl)
            if s:
                return 'c->r[%d] = setnz(c, %s);' % (rd, e)
            return 'c->r[%d] = %s;' % (rd, e)
        if bits(w, 23, 23):                                         # LDREX / STREX family
            size = {0: 32, 1: 64, 2: 8, 3: 16}[bits(w, 22, 21)]
            rn, rt = bits(w, 19, 16), bits(w, 15, 12)
            if bits(w, 20, 20):
                if size == 64:
                    return ('{ uint32_t a = c->r[%d]; c->ex_addr = a; c->ex_val = rd64(a); c->r[%d] = (uint32_t)c->ex_val; '
                            'c->r[%d] = (uint32_t)(c->ex_val >> 32); }' % (rn, rt, rt + 1))
                return '{ uint32_t a = c->r[%d]; c->ex_addr = a; c->ex_val = rd%d(a); c->r[%d] = (uint32_t)c->ex_val; }' % (rn, size, rt)
            rt2 = bits(w, 3, 0)
            if size == 64:
                return 'c->r[%d] = strex64(c, c->r[%d], ((uint64_t)c->r[%d] << 32) | c->r[%d]);' % (rt, rn, rt2 + 1, rt2)
            return 'c->r[%d] = strex%d(c, c->r[%d], (uint%d_t)c->r[%d]);' % (rt, size, rn, size, rt2)
        raise Unimpl('swp')

    # ---- miscellaneous (BX, BLX, CLZ, MRS, MSR, halfword multiplies, saturating add)
    def misc(self, w, pc):
        op2, op = bits(w, 6, 4), bits(w, 22, 21)
        rm = bits(w, 3, 0)
        if bits(w, 7, 7) and not bits(w, 4, 4):
            return self.halfword_mul(w, pc)
        if op2 == 0:
            if op & 1 == 0:                                         # MRS Rd, APSR
                if bits(w, 22, 22):
                    raise Unimpl('mrs-spsr')
                return 'c->r[%d] = (c->N << 31) | (c->Z << 30) | (c->C << 29) | (c->V << 28) | (c->Q << 27) | (c->GE << 16) | 0x10u;' % bits(w, 15, 12)
            mask = bits(w, 19, 18)                                  # MSR APSR, Rn
            s = '{ uint32_t v = %s; ' % R(rm, pc)
            if mask & 2:
                s += 'c->N = v >> 31; c->Z = (v >> 30) & 1; c->C = (v >> 29) & 1; c->V = (v >> 28) & 1; c->Q = (v >> 27) & 1; '
            if mask & 1:
                s += 'c->GE = (v >> 16) & 15; '
            return s + '}'
        if op2 == 1 and op == 1:                                    # BX
            if rm == 14:
                return 'return;'
            return self.write_pc('c->r[%d]' % rm)
        if op2 == 1 and op == 3:
            return 'c->r[%d] = clz32(c->r[%d]);' % (bits(w, 15, 12), rm)
        if op2 == 3 and op == 1:                                    # BLX register
            return '{ uint32_t t = c->r[%d]; c->r[14] = %s; call_indirect(c, t); }' % (rm, u32(pc + 4))
        if op2 == 7 and op == 1:
            return 'guest_trap("bkpt", %s);' % u32(pc)
        if op2 == 5:                                                # QADD, QSUB, QDADD, QDSUB
            rd, rn = bits(w, 15, 12), bits(w, 19, 16)
            b = 'ssat32(c, 2ll * (int32_t)c->r[%d], 32)' % rn if op & 2 else '(int32_t)c->r[%d]' % rn
            sign = '-' if op & 1 else '+'
            return 'c->r[%d] = ssat32(c, (int64_t)(int32_t)c->r[%d] %s (int64_t)(int32_t)%s, 32);' % (rd, rm, sign, b)
        raise Unimpl('misc')

    def halfword_mul(self, w, pc):
        op = bits(w, 22, 21)
        rd, ra, rm, rn = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
        x, y = bits(w, 5, 5), bits(w, 6, 6)
        hn = '(int32_t)(int16_t)(c->r[%d] >> %d)' % (rn, 16 * x)
        hm = '(int32_t)(int16_t)(c->r[%d] >> %d)' % (rm, 16 * y)
        if op == 3:                                                 # SMULxy
            return 'c->r[%d] = (uint32_t)(%s * %s);' % (rd, hn, hm)
        if op == 0:                                                 # SMLAxy
            return ('{ int64_t p = (int64_t)(%s * %s) + (int32_t)c->r[%d]; if (p != (int32_t)p) c->Q = 1; c->r[%d] = (uint32_t)p; }'
                    % (hn, hm, ra, rd))
        if op == 1:                                                 # SMULWy / SMLAWy
            p = '(uint32_t)(((int64_t)(int32_t)c->r[%d] * %s) >> 16)' % (rn, hm)
            if x:
                return 'c->r[%d] = %s;' % (rd, p)
            return ('{ int64_t p = (int64_t)(int32_t)%s + (int32_t)c->r[%d]; if (p != (int32_t)p) c->Q = 1; c->r[%d] = (uint32_t)p; }'
                    % (p, ra, rd))
        # SMLALxy
        return ('{ int64_t p = (int64_t)(%s * %s) + (int64_t)(((uint64_t)c->r[%d] << 32) | c->r[%d]); '
                'c->r[%d] = (uint32_t)p; c->r[%d] = (uint32_t)((uint64_t)p >> 32); }' % (hn, hm, rd, ra, ra, rd))

    # ---- loads and stores
    def address(self, w, pc, offset_expr):
        """(address expr, writeback statement) for P/U/W addressing."""
        p, u, wb, rn = bits(w, 24, 24), bits(w, 23, 23), bits(w, 21, 21), bits(w, 19, 16)
        base = R(rn, pc)
        if rn == 15:
            base = u32((pc + 8) & ~3)
        sign = '+' if u else '-'
        off = '%s %s %s' % (base, sign, offset_expr) if offset_expr != '0u' else base
        if p:
            addr = off
            wbs = ('c->r[%d] = a;' % rn) if wb else ''
        else:
            addr = base
            wbs = 'c->r[%d] = %s;' % (rn, off)
        if rn == 15 and wbs:
            raise Unimpl('ls-pc-writeback')
        return addr, wbs

    def ls_word(self, w, pc, reg):
        b, l, rt, rn = bits(w, 22, 22), bits(w, 20, 20), bits(w, 15, 12), bits(w, 19, 16)
        if reg:
            off, _ = shift_imm(R(bits(w, 3, 0), pc), bits(w, 6, 5), bits(w, 11, 7))
        else:
            off = u32(bits(w, 11, 0))
        addr, wb = self.address(w, pc, off)
        size = 8 if b else 32
        if l:
            if rt == 15:
                pop_ret = (rn == 13 and not bits(w, 24, 24) and bits(w, 23, 23) and not reg and bits(w, 11, 0) == 4)
                if pop_ret:
                    return 'c->r[13] += 4; return;'
                self.has_dispatch = True
                return '{ uint32_t a = %s; uint32_t v = rd32(a); %s jt = v; goto L_dispatch; }' % (addr, wb)
            return '{ uint32_t a = %s; uint32_t v = rd%d(a); %s c->r[%d] = v; }' % (addr, size, wb, rt)
        val = R(rt, pc) if rt != 15 else u32(pc + 8)
        if size == 8:
            val = '(uint8_t)%s' % val
        return '{ uint32_t a = %s; wr%d(a, %s); %s }' % (addr, size, val, wb)

    def extra_ls(self, w, pc):
        op2, l, rt = bits(w, 6, 5), bits(w, 20, 20), bits(w, 15, 12)
        if bits(w, 22, 22):
            off = u32((bits(w, 11, 8) << 4) | bits(w, 3, 0))
        else:
            off = R(bits(w, 3, 0), pc)
        addr, wb = self.address(w, pc, off)
        if rt == 15:
            raise Unimpl('extra-ls-pc')
        if l:
            if op2 == 1:
                return '{ uint32_t a = %s; uint32_t v = rd16(a); %s c->r[%d] = v; }' % (addr, wb, rt)
            if op2 == 2:
                return '{ uint32_t a = %s; uint32_t v = (uint32_t)(int32_t)(int8_t)rd8(a); %s c->r[%d] = v; }' % (addr, wb, rt)
            return '{ uint32_t a = %s; uint32_t v = (uint32_t)(int32_t)(int16_t)rd16(a); %s c->r[%d] = v; }' % (addr, wb, rt)
        if op2 == 1:
            return '{ uint32_t a = %s; wr16(a, (uint16_t)c->r[%d]); %s }' % (addr, rt, wb)
        if op2 == 2:                                                # LDRD
            return '{ uint32_t a = %s; uint32_t v0 = rd32(a), v1 = rd32(a + 4); %s c->r[%d] = v0; c->r[%d] = v1; }' % (addr, wb, rt, rt + 1)
        return '{ uint32_t a = %s; wr32(a, c->r[%d]); wr32(a + 4, c->r[%d]); %s }' % (addr, rt, rt + 1, wb)   # STRD

    def ldm_stm(self, w, pc):
        p, u, s, wb, l, rn = bits(w, 24, 24), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21), bits(w, 20, 20), bits(w, 19, 16)
        if s:
            raise Unimpl('ldm-user')
        regs = [i for i in range(16) if w & (1 << i)]
        n = len(regs)
        if not n:
            raise Unimpl('ldm-empty')
        start = {(0, 1): '0', (1, 1): '4', (0, 0): '%d' % (-4 * n + 4), (1, 0): '%d' % (-4 * n)}[(p, u)]
        out = ['uint32_t a = c->r[%d] + (%s);' % (rn, start) if start != '0' else 'uint32_t a = c->r[%d];' % rn]
        wbs = ('c->r[%d] %s= %d;' % (rn, '+' if u else '-', 4 * n)) if wb else ''
        if l:
            out.append(wbs)
            for i, r in enumerate(regs):
                if r == 15:
                    out.append('uint32_t npc = rd32(a + %d);' % (4 * i))
                else:
                    out.append('c->r[%d] = rd32(a + %d);' % (r, 4 * i))
            if 15 in regs:                                       # compiler-generated code: always a return
                out.append('(void)npc; return;')
        else:
            for i, r in enumerate(regs):
                out.append('wr32(a + %d, %s);' % (4 * i, R(r, pc) if r != 15 else u32(pc + 8)))
            out.append(wbs)
        return '{ ' + ' '.join(x for x in out if x) + ' }'

    # ---- media instructions
    def media(self, w, pc):
        op1, op2 = bits(w, 24, 20), bits(w, 7, 5)
        rd, rn, rm = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0)
        if op1 >> 3 == 1:                                           # 01xxx: packing, unpacking, saturation, reversal
            sub, rot = bits(w, 22, 20), bits(w, 11, 10) * 8
            src = 'ror32(c->r[%d], %d)' % (rm, rot) if rot else 'c->r[%d]' % rm
            ext = {(2, 3): '(uint32_t)(int32_t)(int8_t)%s', (3, 3): '(uint32_t)(int32_t)(int16_t)%s',
                   (6, 3): '(uint32_t)(uint8_t)%s', (7, 3): '(uint32_t)(uint16_t)%s'}
            if (sub, op2) in ext:
                e = ext[(sub, op2)] % src
                if rn != 15:
                    e = 'c->r[%d] + %s' % (rn, e)
                return 'c->r[%d] = %s;' % (rd, e)
            if sub in (0, 4) and op2 == 3:                          # SXTB16 / UXTB16 (and the accumulating forms)
                if sub == 4:
                    e = '(%s & 0x00ff00ffu)' % src
                else:
                    e = '(((uint32_t)(uint16_t)(int16_t)(int8_t)%s) | ((uint32_t)(uint16_t)(int16_t)(int8_t)(%s >> 16) << 16))' % (src, src)
                if rn != 15:
                    e = ('(((c->r[%d] + %s) & 0xffffu) | (((c->r[%d] >> 16) + (%s >> 16)) << 16))' % (rn, e, rn, e))
                return 'c->r[%d] = %s;' % (rd, e)
            if sub == 3 and op2 == 1:
                return 'c->r[%d] = __builtin_bswap32(c->r[%d]);' % (rd, rm)
            if sub == 3 and op2 == 5:
                return 'c->r[%d] = ((c->r[%d] & 0xff00ff00u) >> 8) | ((c->r[%d] & 0x00ff00ffu) << 8);' % (rd, rm, rm)
            if sub == 7 and op2 == 1:
                return 'c->r[%d] = rbit32(c->r[%d]);' % (rd, rm)
            if sub == 7 and op2 == 5:
                return 'c->r[%d] = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)c->r[%d]);' % (rd, rm)
            if sub == 0 and op2 & 1 == 0:                           # PKHBT / PKHTB
                sh = bits(w, 11, 7)
                if bits(w, 6, 6):
                    m = '(uint32_t)((int32_t)c->r[%d] >> %d)' % (rm, sh if sh else 31)
                    return 'c->r[%d] = (c->r[%d] & 0xffff0000u) | (%s & 0xffffu);' % (rd, rn, m)
                return 'c->r[%d] = (c->r[%d] & 0xffffu) | ((c->r[%d] << %d) & 0xffff0000u);' % (rd, rn, rm, sh)
            if op2 & 1 == 0 and sub in (2, 3, 6, 7):                # SSAT / USAT
                sat = bits(w, 20, 16)
                sh, t = bits(w, 11, 7), bits(w, 6, 6)
                if t:
                    v = '(int64_t)((int32_t)c->r[%d] >> %d)' % (rm, sh if sh else 31)
                else:
                    v = '(int64_t)(int32_t)(c->r[%d] << %d)' % (rm, sh)
                if sub & 4:
                    return 'c->r[%d] = usat32(c, %s, %d);' % (rd, v, sat)
                return 'c->r[%d] = ssat32(c, %s, %d);' % (rd, v, sat + 1)
            if sub == 0 and op2 == 5:                               # SEL
                return ('{ uint32_t r = 0; for (int i = 0; i < 4; i++) r |= (((c->GE >> i) & 1) ? c->r[%d] : c->r[%d]) & (0xffu << (8 * i)); c->r[%d] = r; }'
                        % (rn, rm, rd))
            raise Unimpl('media-pack')
        if op1 >> 3 == 0:
            raise Unimpl('media-parallel')
        if op1 in (0x11, 0x13) and op2 == 0:                        # SDIV / UDIV
            rd, rm2, rn2 = bits(w, 19, 16), bits(w, 11, 8), bits(w, 3, 0)
            return 'c->r[%d] = %s(c->r[%d], c->r[%d]);' % (rd, 'sdiv32' if op1 == 0x11 else 'udiv32', rn2, rm2)
        if op1 == 0x15 and op2 in (0, 1):                           # SMMUL / SMMLA (Ra = 15: SMMUL)
            rd, ra, rm2, rn2 = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
            rnd = ' + 0x80000000ll' if op2 else ''
            acc = '((int64_t)c->r[%d] << 32) + ' % ra if ra != 15 else ''
            return 'c->r[%d] = (uint32_t)((uint64_t)(%s(int64_t)(int32_t)c->r[%d] * (int32_t)c->r[%d]%s) >> 32);' % (rd, acc, rn2, rm2, rnd)
        if op1 >> 1 in (0xd, 0xf) and op2 & 3 == 2:                 # SBFX / UBFX
            lsb, width = bits(w, 11, 7), bits(w, 20, 16) + 1
            if op1 >> 1 == 0xd:
                return 'c->r[%d] = (uint32_t)((int32_t)(c->r[%d] << %d) >> %d);' % (rd, rm, 32 - lsb - width, 32 - width)
            return 'c->r[%d] = (c->r[%d] >> %d) & %s;' % (rd, rm, lsb, u32((1 << width) - 1))
        if op1 >> 1 == 0xe and op2 & 3 == 0:                        # BFC / BFI
            lsb, msb = bits(w, 11, 7), bits(w, 20, 16)
            if msb < lsb:
                raise Unimpl('bfi-range')
            mask = ((1 << (msb - lsb + 1)) - 1) << lsb
            if rm == 15:
                return 'c->r[%d] &= %s;' % (rd, u32(~mask))
            return 'c->r[%d] = (c->r[%d] & %s) | ((c->r[%d] << %d) & %s);' % (rd, rd, u32(~mask), rm, lsb, u32(mask))
        if op1 == 0x18 and op2 == 0:                                # USAD8 / USADA8
            rd, ra, rm2, rn2 = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
            acc = 'c->r[%d]' % ra if ra != 15 else '0'
            return ('{ uint32_t s = %s; for (int i = 0; i < 32; i += 8) { int d = (int)((c->r[%d] >> i) & 255) - (int)((c->r[%d] >> i) & 255); s += d < 0 ? -d : d; } c->r[%d] = s; }'
                    % (acc, rn2, rm2, rd))
        if op1 == 0x1f and op2 == 7:
            return 'guest_trap("udf", %s);' % u32(pc)
        raise Unimpl('media')

    # ---- unconditional space
    def uncond(self, w, pc):
        if (w & 0xfffffff0) in (0xf57ff050, 0xf57ff040, 0xf57ff060):    # DMB, DSB, ISB
            return '__sync_synchronize();'
        if w == 0xf57ff01f:                                              # CLREX
            return 'c->ex_addr = ~0u;'
        if (w & 0xfd70f000) == 0xf550f000 or (w & 0xfd70f000) == 0xf510f000 or (w & 0xff70f000) == 0xf450f000:
            return ';'                                                   # PLD / PLDW / PLI
        if bits(w, 27, 25) == 5:
            raise Unimpl('blx-imm-thumb')
        if bits(w, 27, 25) == 1:
            return self.neon_dp(w, pc)
        if bits(w, 27, 24) == 4 and not bits(w, 20, 20):
            return self.neon_ls(w, pc)
        if bits(w, 27, 24) == 0xe and bits(w, 11, 9) == 5 and not bits(w, 4, 4):
            return self.vfp_v8(w, pc)
        raise Unimpl('uncond')

    # ---- ARMv8 VFP additions (unconditional space): VSEL, VMAXNM/VMINNM, VRINT{A,N,P,M}, VCVT{A,N,P,M}
    def vfp_v8(self, w, pc):
        dbl = bits(w, 8, 8)
        D_, N_, M_ = bits(w, 22, 22), bits(w, 7, 7), bits(w, 5, 5)
        vd, vn, vm = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0)
        if dbl:
            d, n, m, Rg = (D_ << 4) | vd, (N_ << 4) | vn, (M_ << 4) | vm, 'D(%d)'
        else:
            d, n, m, Rg = (vd << 1) | D_, (vn << 1) | N_, (vm << 1) | M_, 'S(%d)'
        Dd, Dn, Dm = Rg % d, Rg % n, Rg % m
        if not bits(w, 23, 23) and not bits(w, 6, 6):                # VSEL: cc = EQ, VS, GE, GT
            cc = [0, 6, 10, 12][bits(w, 21, 20)]
            return '%s = cond(c, %d) ? %s : %s;' % (Dd, cc, Dn, Dm)
        if bits(w, 23, 23) and bits(w, 21, 20) == 0:                # VMAXNM / VMINNM
            fn = ('fmin' if bits(w, 6, 6) else 'fmax') + ('' if dbl else 'f')
            return '%s = %s(%s, %s);' % (Dd, fn, Dn, Dm)
        if bits(w, 23, 23) and bits(w, 21, 18) == 0xe and bits(w, 7, 6) == 1:     # VRINT{A,N,P,M}
            fn = ['round', '__builtin_roundeven', 'ceil', 'floor'][bits(w, 17, 16)] + ('' if dbl else 'f')
            return '%s = %s(%s);' % (Dd, fn, Dm)
        if bits(w, 23, 23) and bits(w, 21, 18) == 0xf and bits(w, 6, 6) == 1:     # VCVT{A,N,P,M}: float -> integer, result in an S register
            rnd = ['round', '__builtin_roundeven', 'ceil', 'floor'][bits(w, 17, 16)] + ('' if dbl else 'f')
            sd = (vd << 1) | D_
            return 'c->v.s[%d] = %s(%s(%s));' % (sd, 'f2s32' if bits(w, 7, 7) else 'f2u32', rnd, Dm)
        raise Unimpl('vfp-v8')

    # ---- Advanced SIMD helpers
    @staticmethod
    def qregs(w):
        """(d, n, m) D-register numbers of the Vd/Vn/Vm fields."""
        return (bits(w, 22, 22) << 4) | bits(w, 15, 12), (bits(w, 7, 7) << 4) | bits(w, 19, 16), (bits(w, 5, 5) << 4) | bits(w, 3, 0)

    @staticmethod
    def lanes(expr_fmt, d, n, m, q, lane, count, acc=False, out_lane=None):
        """Elementwise operation: r.<out_lane>[i] = expr, with a, b (and o = old Vd) as V128 operands."""
        out_lane = out_lane or lane
        pre = 'V128 a = vget(c, %d, %d), b = vget(c, %d, %d), r%s;' % (n, q, m, q, (', o = vget(c, %d, %d)' % (d, q)) if acc else '')
        body = expr_fmt.replace('A', 'a.%s[i]' % lane).replace('B', 'b.%s[i]' % lane).replace('O', 'o.%s[i]' % lane)
        return '{ %s for (int i = 0; i < %d; i++) r.%s[i] = %s; vput(c, %d, &r, %d); }' % (pre, count, out_lane, body, d, q)

    def neon_dp(self, w, pc):
        U, q = bits(w, 24, 24), bits(w, 6, 6)
        d, n, m = self.qregs(w)
        if not bits(w, 23, 23):
            return self.neon_3same(w, U, q, d, n, m)
        if bits(w, 4, 4) and bits(w, 21, 19) == 0 and not bits(w, 7, 7):
            return self.neon_imm(w, q, d)
        if bits(w, 4, 4):
            return self.neon_shift(w, U, q, d, m)
        if bits(w, 21, 20) != 3:
            if bits(w, 6, 6):
                return self.neon_scalar(w, U, d, n, m)
            raise Unimpl('neon-3diff')
        if not U:                                                    # VEXT
            imm4 = bits(w, 11, 8)
            nb = 16 if q else 8
            return ('{ V128 a = vget(c, %d, %d), b = vget(c, %d, %d), r; for (int i = 0; i < %d; i++) '
                    'r.u8[i] = (i + %d < %d) ? a.u8[i + %d] : b.u8[i + %d - %d]; vput(c, %d, &r, %d); }'
                    % (n, q, m, q, nb, imm4, nb, imm4, imm4, nb, d, q))
        if bits(w, 11, 10) < 2:
            return self.neon_2misc(w, q, d, m)
        if bits(w, 11, 10) == 2:                                     # VTBL / VTBX
            length = bits(w, 9, 8) + 1
            tbx = bits(w, 6, 6)
            return ('{ uint8_t t[32]; for (int k = 0; k < %d; k++) memcpy(t + 8 * k, &c->v.d[(%d + k) & 31], 8); '
                    'V128 x = vget(c, %d, 0), o = vget(c, %d, 0), r; for (int i = 0; i < 8; i++) '
                    'r.u8[i] = x.u8[i] < %d ? t[x.u8[i]] : %s; vput(c, %d, &r, 0); }'
                    % (length, n, m, d, 8 * length, 'o.u8[i]' if tbx else '0', d))
        if bits(w, 11, 7) == 0x18:                                   # VDUP scalar
            imm4 = bits(w, 19, 16)
            if imm4 & 1:
                lane, idx, cnt = 'u8', imm4 >> 1, 16
            elif imm4 & 2:
                lane, idx, cnt = 'u16', imm4 >> 2, 8
            elif imm4 & 4:
                lane, idx, cnt = 'u32', imm4 >> 3, 4
            else:
                raise Unimpl('vdup-scalar')
            cnt = cnt if q else cnt // 2
            return '{ V128 a = vget(c, %d, 0), r; for (int i = 0; i < %d; i++) r.%s[i] = a.%s[%d]; vput(c, %d, &r, %d); }' % (m, cnt, lane, lane, idx, d, q)
        raise Unimpl('neon-other')

    def neon_3same(self, w, U, q, d, n, m):
        opc, o1, size = bits(w, 11, 8), bits(w, 4, 4), bits(w, 21, 20)
        esz = 8 << size
        cnt = (128 if q else 64) // esz
        ul, sl = 'u%d' % esz, 's%d' % esz
        L = lambda e, lane, acc=False, count=None: self.lanes(e, d, n, m, q, lane, count or cnt, acc)
        if opc == 1 and o1:                                          # bitwise
            e = {(0, 0): 'A & B', (0, 1): 'A & ~B', (0, 2): 'A | B', (0, 3): 'A | ~B', (1, 0): 'A ^ B',
                 (1, 1): '(O & A) | (~O & B)', (1, 2): '(O & ~B) | (A & B)', (1, 3): '(O & B) | (A & ~B)'}[(U, size)]
            return self.lanes(e, d, n, m, q, 'u64', 2 if q else 1, acc=U == 1 and size != 0)
        if opc == 8 and not o1:
            return L('A %s B' % ('-' if U else '+'), ul)
        if opc == 8 and o1:
            return L('(A == B) ? (uint%d_t)-1 : 0' % esz if U else '(A & B) ? (uint%d_t)-1 : 0' % esz, ul)
        if opc == 9 and not o1:
            return L('O %s A * B' % ('-' if U else '+'), ul, acc=True)
        if opc == 9 and o1 and not U:
            return L('A * B', ul)
        if opc == 3:
            lane = ul if U else sl
            return self.lanes('(A %s B) ? (uint%d_t)-1 : 0' % ('>=' if o1 else '>', esz), d, n, m, q, lane, cnt, out_lane=ul)
        if opc == 6:
            lane = ul if U else sl
            return L('(A %s B) ? A : B' % ('<' if o1 else '>'), lane)
        if opc == 0xb and o1 and not U and not q:                    # VPADD integer (D only)
            half = cnt // 2
            return ('{ V128 a = vget(c, %d, 0), b = vget(c, %d, 0), r; for (int i = 0; i < %d; i++) { r.%s[i] = a.%s[2*i] + a.%s[2*i+1]; '
                    'r.%s[i + %d] = b.%s[2*i] + b.%s[2*i+1]; } vput(c, %d, &r, 0); }' % (n, m, half, ul, ul, ul, ul, half, ul, ul, d))
        # floating point (32-bit lanes only; size bit 20 = 1 would be f16)
        if opc >= 0xc:
            if size & 1:
                raise Unimpl('neon-f16')
            op = size >> 1
            fc = 4 if q else 2
            F = lambda e, acc=False: self.lanes(e, d, n, m, q, 'f32', fc, acc)
            if opc == 0xd and not o1 and not U:
                return F('A %s B' % ('-' if op else '+'))
            if opc == 0xd and not o1 and U and not op:               # VPADD.F32 (D only)
                if q:
                    raise Unimpl('vpadd-f-q')
                return '{ V128 a = vget(c, %d, 0), b = vget(c, %d, 0), r; r.f32[0] = a.f32[0] + a.f32[1]; r.f32[1] = b.f32[0] + b.f32[1]; vput(c, %d, &r, 0); }' % (n, m, d)
            if opc == 0xd and not o1 and U and op:
                return F('fabsf(A - B)')
            if opc == 0xd and o1 and not U:
                return F('O %s A * B' % ('-' if op else '+'), acc=True)
            if opc == 0xd and o1 and U and not op:
                return F('A * B')
            if opc == 0xc and o1 and not U:                          # VFMA / VFMS
                return F('fmaf(%sA, B, O)' % ('-' if op else ''), acc=True)
            if opc == 0xe and not o1:
                cmp = {(0, 0): '==', (1, 0): '>=', (1, 1): '>'}.get((U, op))
                if cmp:
                    return self.lanes('(A %s B) ? 0xffffffffu : 0' % cmp, d, n, m, q, 'f32', fc, out_lane='u32')
            if opc == 0xe and o1 and U:                              # VACGE / VACGT
                return self.lanes('(fabsf(A) %s fabsf(B)) ? 0xffffffffu : 0' % ('>' if op else '>='), d, n, m, q, 'f32', fc, out_lane='u32')
            if opc == 0xf and not o1 and not U:
                return F('%s(A, B)' % ('fminf_arm' if op else 'fmaxf_arm'))
            if opc == 0xf and not o1 and U:                          # VPMAX / VPMIN (D only)
                if q:
                    raise Unimpl('vpmax-q')
                f = 'fminf_arm' if op else 'fmaxf_arm'
                return ('{ V128 a = vget(c, %d, 0), b = vget(c, %d, 0), r; r.f32[0] = %s(a.f32[0], a.f32[1]); r.f32[1] = %s(b.f32[0], b.f32[1]); vput(c, %d, &r, 0); }'
                        % (n, m, f, f, d))
            if opc == 0xf and o1 and not U:                          # VRECPS / VRSQRTS
                return F('(3.0f - A * B) * 0.5f' if op else '2.0f - A * B')
            if opc == 0xf and o1 and U:                              # VMAXNM / VMINNM (ARMv8)
                return F('%s(A, B)' % ('fminf' if op else 'fmaxf'))
        raise Unimpl('neon-3same')

    def neon_imm(self, w, q, d):
        cmode, op = bits(w, 11, 8), bits(w, 5, 5)
        imm8 = (bits(w, 24, 24) << 7) | (bits(w, 18, 16) << 4) | bits(w, 3, 0)
        if cmode >> 1 in (0, 1, 2, 3):
            v = (imm8 << (8 * (cmode >> 1))) * 0x0000000100000001
        elif cmode >> 1 in (4, 5):
            v = (imm8 << (8 * ((cmode >> 1) & 1))) * 0x0001000100010001
        elif cmode == 0xc:
            v = ((imm8 << 8) | 0xff) * 0x0000000100000001
        elif cmode == 0xd:
            v = ((imm8 << 16) | 0xffff) * 0x0000000100000001
        elif cmode == 0xe and not op:
            v = imm8 * 0x0101010101010101
        elif cmode == 0xe and op:
            v = 0
            for i in range(8):
                if imm8 & (1 << i):
                    v |= 0xff << (8 * i)
        elif cmode == 0xf and not op:
            a, b, cd, efgh = imm8 >> 7, (imm8 >> 6) & 1, (imm8 >> 4) & 3, imm8 & 15
            f = (a << 31) | ((b ^ 1) << 30) | ((0x1f if b else 0) << 25) | (cd << 23) | (efgh << 19)
            v = f * 0x0000000100000001
        else:
            raise Unimpl('vmov-imm')
        v &= (1 << 64) - 1
        if cmode < 0xc and cmode & 1:                                # VORR / VBIC immediate
            e = 'c->v.d[%d] %s 0x%xull;' % (d, '&= ~' if op else '|=', v)
            e = e.replace('&= ~ 0x', '&= ~0x')
            return e + (' ' + e.replace('[%d]' % d, '[%d]' % (d + 1)) if q else '')
        if op and cmode != 0xe:                                      # VMVN
            v = ~v & ((1 << 64) - 1)
        s = 'c->v.d[%d] = 0x%xull;' % (d, v)
        return s + (' c->v.d[%d] = 0x%xull;' % (d + 1, v) if q else '')

    def neon_shift(self, w, U, q, d, m):
        opc, L_ = bits(w, 11, 8), bits(w, 7, 7)
        imm6 = bits(w, 21, 16)
        if L_:
            esz, shift_r, shift_l = 64, 64 - imm6, imm6
        elif imm6 >= 32:
            esz, shift_r, shift_l = 32, 64 - imm6, imm6 - 32
        elif imm6 >= 16:
            esz, shift_r, shift_l = 16, 32 - imm6, imm6 - 16
        elif imm6 >= 8:
            esz, shift_r, shift_l = 8, 16 - imm6, imm6 - 8
        else:
            raise Unimpl('neon-shift-size')
        cnt = (128 if q else 64) // esz
        ul, sl = 'u%d' % esz, 's%d' % esz
        if opc in (0, 1):                                            # VSHR / VSRA
            lane = ul if U else sl
            e = 'A >> %d' % shift_r if shift_r < esz else ('0' if U else 'A >> %d' % (esz - 1))
            if opc == 1:
                return self.lanes('O + (%s)' % e, d, m, m, q, lane, cnt, acc=True)
            return self.lanes(e, d, m, m, q, lane, cnt)
        if opc == 5 and not U:                                       # VSHL immediate
            return self.lanes('A << %d' % shift_l, d, m, m, q, ul, cnt)
        if opc == 0xa and not L_ and shift_l >= 0 and not q:         # VSHLL / VMOVL (Q result from a D source)
            src = ('u%d' if U else 's%d') % esz
            dst = ('u%d' if U else 's%d') % (esz * 2)
            return ('{ V128 a = vget(c, %d, 0), r; for (int i = 0; i < %d; i++) r.%s[i] = (%s)a.%s[i] << %d; vput(c, %d, &r, 1); }'
                    % (m, 64 // esz, dst, 'int64_t' if not U else 'uint64_t', src, shift_l, d))
        if opc == 8 and not U and not bits(w, 6, 6):                 # VSHRN (narrow)
            if esz == 64:                                            # imm6 encodes the destination size here
                raise Unimpl('vshrn')
            src, sr = 'u%d' % (esz * 2), shift_r
            return ('{ V128 a = vget(c, %d, 1), r; for (int i = 0; i < %d; i++) r.u%d[i] = (uint%d_t)(a.%s[i] >> %d); vput(c, %d, &r, 0); }'
                    % (m, 64 // esz, esz, esz, src, sr, d))
        if opc in (0xe, 0xf) and esz == 32 and L_ == 0:              # VCVT fixed <-> float, fbits = 64 - imm6
            fbits = 64 - imm6
            to_fixed, unsigned = opc & 1, U
            scale = float(1 << fbits)
            if to_fixed:
                conv = 'f2u32' if unsigned else 'f2s32'
                return self.lanes('%s((double)A * %r)' % (conv, scale), d, m, m, q, 'f32', cnt, out_lane='u32')
            return self.lanes('(float)((double)A / %r)' % scale, d, m, m, q, 'u32' if unsigned else 's32', cnt, out_lane='f32')
        raise Unimpl('neon-shift')

    def neon_scalar(self, w, Q, d, n, m):
        opc, size = bits(w, 11, 8), bits(w, 21, 20)
        q = Q                                                        # bit 24 is Q for the scalar forms
        if size == 2:
            mm, idx = bits(w, 3, 0), bits(w, 5, 5)
        elif size == 1:
            mm, idx = bits(w, 2, 0), (bits(w, 5, 5) << 1) | bits(w, 3, 3)
        else:
            raise Unimpl('neon-scalar-size')
        esz = 8 << size
        cnt = (128 if q else 64) // esz
        lane = 'f32' if opc & 1 and opc in (1, 5, 9) else 'u%d' % esz
        if opc in (1, 5, 9) and size != 2:
            raise Unimpl('neon-scalar-f16')
        e = {0: 'O + A * X', 1: 'O + A * X', 4: 'O - A * X', 5: 'O - A * X', 8: 'A * X', 9: 'A * X'}.get(opc)
        if e is None:
            raise Unimpl('neon-scalar')
        acc = opc in (0, 1, 4, 5)
        pre = 'V128 a = vget(c, %d, %d), x = vget(c, %d, 0), r%s;' % (n, q, mm, (', o = vget(c, %d, %d)' % (d, q)) if acc else '')
        body = e.replace('A', 'a.%s[i]' % lane).replace('O', 'o.%s[i]' % lane).replace('X', 'x.%s[%d]' % (lane, idx))
        return '{ %s for (int i = 0; i < %d; i++) r.%s[i] = %s; vput(c, %d, &r, %d); }' % (pre, cnt, lane, body, d, q)

    def neon_2misc(self, w, q, d, m):
        A, B, size = bits(w, 17, 16), bits(w, 10, 7), bits(w, 19, 18)
        esz = 8 << size
        cnt = (128 if q else 64) // esz
        ul, sl = 'u%d' % esz, 's%d' % esz
        one = lambda e, lane, out=None: self.lanes(e, d, m, m, q, lane, cnt, out_lane=out)
        if A == 0:
            if B == 0xb:
                return self.lanes('~A', d, m, m, q, 'u64', 2 if q else 1)
            if B in (0, 1, 2):                                       # VREV64 / VREV32 / VREV16
                group = {0: 8, 1: 4, 2: 2}[B]                        # group size in bytes
                eb = esz // 8
                return ('{ V128 a = vget(c, %d, %d), r; for (int i = 0; i < %d; i += %d) for (int j = 0; j < %d; j += %d) '
                        'memcpy(&r.u8[i + j], &a.u8[i + %d - %d - j], %d); vput(c, %d, &r, %d); }'
                        % (m, q, 16 if q else 8, group, group, eb, group, eb, eb, d, q))
            if B == 9:
                return one('(uint%d_t)(clz32((uint32_t)A) - %d)' % (esz, 32 - esz) if esz < 32 else 'clz32(A)', ul)
            if B == 0xa and size == 0:
                return one('(uint8_t)__builtin_popcount(A)', 'u8')
        if A == 1:
            F, op = bits(w, 10, 10), bits(w, 9, 7)
            if F:
                if size != 2:
                    raise Unimpl('neon-2misc-f16')
                cmp = {0: '>', 1: '>=', 2: '==', 3: '<=', 4: '<'}.get(op)
                if cmp:
                    return one('(A %s 0.0f) ? 0xffffffffu : 0' % cmp, 'f32', 'u32')
                if op == 6:
                    return one('fabsf(A)', 'f32')
                if op == 7:
                    return one('-A', 'f32')
            else:
                cmp = {0: '>', 1: '>=', 2: '==', 3: '<=', 4: '<'}.get(op)
                if cmp:
                    return one('(A %s 0) ? (uint%d_t)-1 : 0' % (cmp, esz), sl, ul)
                if op == 6:
                    return one('A < 0 ? -A : A', sl)
                if op == 7:
                    return one('-A', sl)
        if A == 2:
            if B in (0, 1, 2, 3) and B != 0:                         # VTRN / VUZP / VZIP
                kind = {1: 'trn', 2: 'uzp', 3: 'zip'}[B]
                return self.neon_permute(kind, q, d, m, esz)
            if B == 0:                                               # VSWP
                if q:
                    return '{ uint64_t t0 = c->v.d[%d], t1 = c->v.d[%d]; c->v.d[%d] = c->v.d[%d]; c->v.d[%d] = c->v.d[%d]; c->v.d[%d] = t0; c->v.d[%d] = t1; }' % (d, d + 1, d, m, d + 1, m + 1, m, m + 1)
                return '{ uint64_t t = c->v.d[%d]; c->v.d[%d] = c->v.d[%d]; c->v.d[%d] = t; }' % (d, d, m, m)
            if B == 4 and not bits(w, 6, 6):                         # VMOVN
                src = 'u%d' % (esz * 2)
                return ('{ V128 a = vget(c, %d, 1), r; for (int i = 0; i < %d; i++) r.%s[i] = (uint%d_t)a.%s[i]; vput(c, %d, &r, 0); }'
                        % (m, 64 // esz, ul, esz, src, d))
        if A == 3:
            if bits(w, 10, 9) == 3 and size == 2:                    # VCVT between f32 and integer
                op = bits(w, 8, 7)
                if op == 0:
                    return one('(float)A', 's32', 'f32')
                if op == 1:
                    return one('(float)A', 'u32', 'f32')
                if op == 2:
                    return one('f2s32(A)', 'f32', 'u32')
                return one('f2u32(A)', 'f32', 'u32')
            if bits(w, 10, 9) == 2 and size == 2 and bits(w, 8, 8):     # VRECPE / VRSQRTE (float)
                return one('frsqrte(A)' if bits(w, 7, 7) else 'frecpe(A)', 'f32')
        if A == 2 and bits(w, 11, 10) == 1 and size == 2:            # VRINT{N,X,A,Z,M,P} (ARMv8)
            fn = {0: '__builtin_roundevenf', 1: 'nearbyintf', 2: 'roundf', 3: 'truncf', 5: 'floorf', 7: 'ceilf'}.get(bits(w, 9, 7))
            if fn:
                return one('%s(A)' % fn, 'f32')
        if A == 3 and bits(w, 11, 10) == 0 and size == 2:            # VCVT{A,N,P,M} (ARMv8)
            rm, uns = bits(w, 9, 8), bits(w, 7, 7)
            fn = ['roundf', '__builtin_roundevenf', 'ceilf', 'floorf'][rm]
            return one('%s(%s(A))' % ('f2u32' if uns else 'f2s32', fn), 'f32', 'u32')
        raise Unimpl('neon-2misc')

    def neon_permute(self, kind, q, d, m, esz):
        n = (128 if q else 64) // esz
        lane = 'u%d' % esz
        if kind == 'zip':
            return ('{ V128 a = vget(c, %d, %d), b = vget(c, %d, %d), lo, hi; for (int i = 0; i < %d; i++) { int k = 2 * i; '
                    'if (k < %d) { lo.%s[k] = a.%s[i]; lo.%s[k+1] = b.%s[i]; } else { hi.%s[k-%d] = a.%s[i]; hi.%s[k+1-%d] = b.%s[i]; } } '
                    'vput(c, %d, &lo, %d); vput(c, %d, &hi, %d); }'
                    % (d, q, m, q, n, n, lane, lane, lane, lane, lane, n, lane, lane, n, lane, d, q, m, q))
        if kind == 'uzp':
            return ('{ V128 a = vget(c, %d, %d), b = vget(c, %d, %d), lo, hi; for (int i = 0; i < %d; i++) { int k = 2 * i; '
                    'lo.%s[i] = k < %d ? a.%s[k] : b.%s[k - %d]; hi.%s[i] = k + 1 < %d ? a.%s[k + 1] : b.%s[k + 1 - %d]; } '
                    'vput(c, %d, &lo, %d); vput(c, %d, &hi, %d); }'
                    % (d, q, m, q, n, lane, n, lane, lane, n, lane, n, lane, lane, n, d, q, m, q))
        return ('{ V128 a = vget(c, %d, %d), b = vget(c, %d, %d); for (int i = 0; i < %d; i += 2) { uint64_t t = a.%s[i + 1]; '
                'a.%s[i + 1] = b.%s[i]; b.%s[i] = t; } vput(c, %d, &a, %d); vput(c, %d, &b, %d); }'
                % (d, q, m, q, n, lane, lane, lane, lane, d, q, m, q))

    def neon_ls(self, w, pc):
        """VLDn / VSTn. Implemented: VLD1/VST1 multiple, VLD1/VST1 single lane, VLD1 all lanes; others trap."""
        A, L_, rn, rm = bits(w, 23, 23), bits(w, 21, 21), bits(w, 19, 16), bits(w, 3, 0)
        d = (bits(w, 22, 22) << 4) | bits(w, 15, 12)
        if rn == 15:
            raise Unimpl('vld-pc')
        out = []
        if not A:
            typ = bits(w, 11, 8)
            nregs = {7: 1, 0xa: 2, 6: 3, 2: 4}.get(typ)
            if nregs is None:
                kind = {8: (2, 1), 9: (2, 1), 3: (2, 2), 4: (3, 1), 5: (3, 2), 0: (4, 1), 1: (4, 2)}.get(typ)
                if kind is None:
                    raise Unimpl('vldn-type')
                return self.neon_ldn(w, d, rn, rm, L_, kind)
            size = 8 * nregs
            for i in range(nregs):
                if L_:
                    out.append('c->v.d[%d] = rd64(a + %d);' % ((d + i) & 31, 8 * i))
                else:
                    out.append('wr64(a + %d, c->v.d[%d]);' % (8 * i, (d + i) & 31))
        else:
            sz = bits(w, 11, 10)
            if sz == 3:                                              # VLDn all lanes
                if not L_ or bits(w, 9, 8) != 0:
                    raise Unimpl('vld-all-n')
                esz = 8 << bits(w, 7, 6)
                nregs = 2 if bits(w, 5, 5) else 1
                rep = {8: 0x0101010101010101, 16: 0x0001000100010001, 32: 0x0000000100000001}[esz]
                out.append('uint64_t v = 0x%xull * rd%d(a);' % (rep, esz))
                for i in range(nregs):
                    out.append('c->v.d[%d] = v;' % ((d + i) & 31))
                size = esz // 8
            else:
                if bits(w, 9, 8) != 0:
                    raise Unimpl('vldn-lane')
                esz = 8 << sz
                idx = bits(w, 7, 5 + sz)
                size = esz // 8
                sh = idx * esz
                mask = (1 << esz) - 1
                if L_:
                    out.append('c->v.d[%d] = (c->v.d[%d] & ~(0x%xull << %d)) | ((uint64_t)rd%d(a) << %d);' % (d, d, mask, sh, esz, sh))
                else:
                    out.append('wr%d(a, (uint%d_t)(c->v.d[%d] >> %d));' % (esz, esz, d, sh))
        wb = ''
        if rm == 13:
            wb = 'c->r[%d] += %d;' % (rn, size)
        elif rm != 15:
            wb = 'c->r[%d] += c->r[%d];' % (rn, rm)
        return '{ uint32_t a = c->r[%d]; %s %s }' % (rn, ' '.join(out), wb)

    def neon_ldn(self, w, d, rn, rm, load, kind):
        """VLD2-4 / VST2-4 multiple structures (interleaved)."""
        nstruct, inc = kind
        esz = 8 << bits(w, 7, 6)
        per = 64 // esz
        regs = [(d + i * inc) & 31 for i in range(nstruct)]
        total = 8 * nstruct
        lines = []
        for e in range(per):
            for s, r in enumerate(regs):
                off = (e * nstruct + s) * (esz // 8)
                if load:
                    lines.append('c->v.d[%d] = (c->v.d[%d] & ~(0x%xull << %d)) | ((uint64_t)rd%d(a + %d) << %d);'
                                 % (r, r, (1 << esz) - 1, e * esz, esz, off, e * esz))
                else:
                    lines.append('wr%d(a + %d, (uint%d_t)(c->v.d[%d] >> %d));' % (esz, off, esz, r, e * esz))
        wb = ''
        if rm == 13:
            wb = 'c->r[%d] += %d;' % (rn, total)
        elif rm != 15:
            wb = 'c->r[%d] += c->r[%d];' % (rn, rm)
        return '{ uint32_t a = c->r[%d]; %s %s }' % (rn, ' '.join(lines), wb)

    # ---- VFP: loads and stores, 64-bit moves
    def coproc_ls(self, w, pc):
        cp = bits(w, 11, 8)
        if cp not in (10, 11):
            raise Unimpl('cop-ls')
        dbl = cp == 11
        if bits(w, 24, 21) == 2:                                     # VMOV 2 core regs <-> D / 2 S (MCRR / MRRC)
            to_core, rt2, rt = bits(w, 20, 20), bits(w, 19, 16), bits(w, 15, 12)
            m = bits(w, 3, 0)
            mbit = bits(w, 5, 5)
            if dbl:
                dm = (mbit << 4) | m
                if to_core:
                    return 'c->r[%d] = (uint32_t)c->v.d[%d]; c->r[%d] = (uint32_t)(c->v.d[%d] >> 32);' % (rt, dm, rt2, dm)
                return 'c->v.d[%d] = ((uint64_t)c->r[%d] << 32) | c->r[%d];' % (dm, rt2, rt)
            sm = (m << 1) | mbit
            if to_core:
                return 'c->r[%d] = c->v.s[%d]; c->r[%d] = c->v.s[%d];' % (rt, sm, rt2, sm + 1)
            return 'c->v.s[%d] = c->r[%d]; c->v.s[%d] = c->r[%d];' % (sm, rt, sm + 1, rt2)
        p, u, dbit, wb, l, rn = bits(w, 24, 24), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21), bits(w, 20, 20), bits(w, 19, 16)
        vd, imm8 = bits(w, 15, 12), bits(w, 7, 0)
        first = ((dbit << 4) | vd) if dbl else ((vd << 1) | dbit)
        base = u32((pc + 8) & ~3) if rn == 15 else 'c->r[%d]' % rn
        if p and not wb:                                             # VLDR / VSTR
            a = '%s %s %d' % (base, '+' if u else '-', imm8 * 4)
            if dbl:
                if l:
                    return 'c->v.d[%d] = rd64(%s);' % (first, a)
                return 'wr64(%s, c->v.d[%d]);' % (a, first)
            if l:
                return 'c->v.s[%d] = rd32(%s);' % (first, a)
            return 'wr32(%s, c->v.s[%d]);' % (a, first)
        # VLDM / VSTM / VPUSH / VPOP
        if p == u:
            raise Unimpl('vldm-mode')
        count = imm8 // 2 if dbl else imm8
        size = 8 if dbl else 4
        out = ['uint32_t a = %s%s;' % (base, '' if u else ' - %d' % (imm8 * 4))]
        for i in range(count):
            r = first + i
            if dbl:
                out.append(('c->v.d[%d] = rd64(a + %d);' if l else 'wr64(a + %d, c->v.d[%d]);') % ((r, i * size) if l else (i * size, r)))
            else:
                out.append(('c->v.s[%d] = rd32(a + %d);' if l else 'wr32(a + %d, c->v.s[%d]);') % ((r, i * size) if l else (i * size, r)))
        if wb:
            if rn == 15:
                raise Unimpl('vldm-pc-wb')
            out.append('c->r[%d] %s= %d;' % (rn, '+' if u else '-', imm8 * 4))
        return '{ ' + ' '.join(out) + ' }'

    # ---- coprocessor register transfers and VFP data processing
    def coproc(self, w, pc):
        cp = bits(w, 11, 8)
        if bits(w, 4, 4):                                            # MRC / MCR
            l, rt = bits(w, 20, 20), bits(w, 15, 12)
            if cp == 15:
                opc1, crn, crm, opc2 = bits(w, 23, 21), bits(w, 19, 16), bits(w, 3, 0), bits(w, 7, 5)
                if (opc1, crn, crm) == (0, 13, 0) and opc2 in (2, 3):
                    reg = 'c->tpidrurw' if opc2 == 2 else 'c->tpidruro'
                    return ('c->r[%d] = %s;' % (rt, reg)) if l else ('%s = c->r[%d];' % (reg, rt))
                raise Unimpl('cp15')
            if cp == 10:
                op = bits(w, 23, 21)
                if op == 0:                                          # VMOV core <-> single
                    sn = (bits(w, 19, 16) << 1) | bits(w, 7, 7)
                    return ('c->r[%d] = c->v.s[%d];' % (rt, sn)) if l else ('c->v.s[%d] = %s;' % (sn, R(rt, pc)))
                if op == 7 and bits(w, 19, 16) == 1:                 # VMRS / VMSR FPSCR
                    if l:
                        if rt == 15:
                            return 'c->N = c->fN; c->Z = c->fZ; c->C = c->fC; c->V = c->fV;'
                        return 'c->r[%d] = fpscr_get(c);' % rt
                    return 'fpscr_set(c, c->r[%d]);' % rt
                if op == 7 and l:                                    # VMRS other system registers (FPSID, MVFR)
                    return 'c->r[%d] = 0;' % rt
                raise Unimpl('vmrs')
            if cp == 11:                                             # VMOV scalar <-> core, VDUP
                opc1, opc2 = bits(w, 22, 21), bits(w, 6, 5)
                dn = (bits(w, 7, 7) << 4) | bits(w, 19, 16)
                if l:
                    u = bits(w, 23, 23)
                    if opc1 & 2 == 0 and opc2 == 0 and not u:
                        return 'c->r[%d] = c->v.s[%d];' % (rt, dn * 2 + (opc1 & 1))
                    if opc1 & 2:                                     # byte
                        idx = ((opc1 & 1) << 2) | opc2
                        cast = '(uint32_t)(uint8_t)' if u else '(uint32_t)(int32_t)(int8_t)'
                        return 'c->r[%d] = %s(c->v.d[%d] >> %d);' % (rt, cast, dn, idx * 8)
                    if opc2 & 1:                                     # halfword
                        idx = ((opc1 & 1) << 1) | (opc2 >> 1)
                        cast = '(uint32_t)(uint16_t)' if u else '(uint32_t)(int32_t)(int16_t)'
                        return 'c->r[%d] = %s(c->v.d[%d] >> %d);' % (rt, cast, dn, idx * 16)
                    raise Unimpl('vmov-scalar')
                if bits(w, 23, 23):                                  # VDUP from core
                    b, e, q = bits(w, 22, 22), bits(w, 5, 5), bits(w, 21, 21)
                    if b:
                        v = '0x0101010101010101ull * (uint8_t)c->r[%d]' % rt
                    elif e:
                        v = '0x0001000100010001ull * (uint16_t)c->r[%d]' % rt
                    else:
                        v = '0x0000000100000001ull * c->r[%d]' % rt
                    s = 'c->v.d[%d] = %s;' % (dn, v)
                    if q:
                        s = '{ uint64_t t = %s; c->v.d[%d] = t; c->v.d[%d] = t; }' % (v, dn, dn + 1)
                    return s
                if opc1 & 2 == 0 and opc2 == 0:
                    return 'c->v.s[%d] = %s;' % (dn * 2 + (opc1 & 1), R(rt, pc))
                if opc1 & 2:
                    idx = ((opc1 & 1) << 2) | opc2
                    return 'c->v.d[%d] = (c->v.d[%d] & ~(0xffull << %d)) | ((uint64_t)(uint8_t)c->r[%d] << %d);' % (dn, dn, idx * 8, rt, idx * 8)
                if opc2 & 1:
                    idx = ((opc1 & 1) << 1) | (opc2 >> 1)
                    return 'c->v.d[%d] = (c->v.d[%d] & ~(0xffffull << %d)) | ((uint64_t)(uint16_t)c->r[%d] << %d);' % (dn, dn, idx * 16, rt, idx * 16)
                raise Unimpl('vmov-scalar')
            raise Unimpl('mrc')
        if cp in (10, 11):
            return self.vfp_dp(w, pc, cp == 11)
        raise Unimpl('cdp')

    def vfp_dp(self, w, pc, dbl):
        opc1, opc2, opc3 = bits(w, 23, 20), bits(w, 19, 16), bits(w, 7, 6)
        D_, N_, M_ = bits(w, 22, 22), bits(w, 7, 7), bits(w, 5, 5)
        vd, vn, vm = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0)
        if dbl:
            d, n, m = (D_ << 4) | vd, (N_ << 4) | vn, (M_ << 4) | vm
            Rg = 'D(%d)'
            T = 'double'
        else:
            d, n, m = (vd << 1) | D_, (vn << 1) | N_, (vm << 1) | M_
            Rg = 'S(%d)'
            T = 'float'
        Dd, Dn, Dm = Rg % d, Rg % n, Rg % m
        op = bits(w, 6, 6)
        key = opc1 & 0xb
        if key == 0:
            return '%s = %s %s %s * %s;' % (Dd, Dd, '-' if op else '+', Dn, Dm)
        if key == 1:                                                 # VNMLS (op 0) / VNMLA (op 1)
            return ('%s = -%s %s %s * %s;' % (Dd, Dd, '-' if op else '+', Dn, Dm))
        if key == 2:
            return '%s = %s%s * %s;' % (Dd, '-' if op else '', Dn, Dm) if op else '%s = %s * %s;' % (Dd, Dn, Dm)
        if key == 3:
            return '%s = %s %s %s;' % (Dd, Dn, '-' if op else '+', Dm)
        if key == 8 and not op:
            return '%s = %s / %s;' % (Dd, Dn, Dm)
        if key in (9, 10):                                           # VFNMA/VFNMS, VFMA/VFMS
            fn = 'fma' if dbl else 'fmaf'
            neg_prod = '-' if op else ''
            if key == 10:
                return '%s = %s(%s%s, %s, %s);' % (Dd, fn, neg_prod, Dn, Dm, Dd)
            return '%s = %s(%s%s, %s, -%s);' % (Dd, fn, '-' if op else '', Dn, Dm, Dd)
        if key != 11:
            raise Unimpl('vfp-dp')
        if opc3 & 1 == 0:                                            # VMOV immediate
            imm8 = (bits(w, 19, 16) << 4) | bits(w, 3, 0)
            a, b, cd, efgh = imm8 >> 7, (imm8 >> 6) & 1, (imm8 >> 4) & 3, imm8 & 15
            if dbl:
                v = (a << 63) | ((b ^ 1) << 62) | ((0xff if b else 0) << 54) | (cd << 52) | (efgh << 48)
                return 'c->v.d[%d] = 0x%xull;' % (d, v)
            v = (a << 31) | ((b ^ 1) << 30) | ((0x1f if b else 0) << 25) | (cd << 23) | (efgh << 19)
            return 'c->v.s[%d] = 0x%xu;' % (d, v)
        if opc2 == 0:
            return ('%s = %s;' % (Dd, Dm)) if opc3 == 1 else ('%s = %s(%s);' % (Dd, 'fabs' if dbl else 'fabsf', Dm))
        if opc2 == 1:
            return ('%s = -%s;' % (Dd, Dm)) if opc3 == 1 else ('%s = %s(%s);' % (Dd, 'sqrt' if dbl else 'sqrtf', Dm))
        if opc2 in (4, 5):                                           # VCMP / VCMPE
            other = Dm if opc2 == 4 else '0.0'
            return 'vcmp(c, %s, %s);' % (Dd, other)
        if opc2 == 7 and opc3 == 3:                                  # VCVT between single and double
            if dbl:
                return 'S(%d) = (float)%s;' % ((vd << 1) | D_, Dm)
            return 'D(%d) = (double)%s;' % ((D_ << 4) | vd, Dm)
        if opc2 == 8:                                                # VCVT integer -> float (source is an S register)
            sm = (vm << 1) | M_
            src = '(int32_t)c->v.s[%d]' % sm if bits(w, 7, 7) else 'c->v.s[%d]' % sm
            return '%s = (%s)%s;' % (Dd, T, src)
        if opc2 in (12, 13):                                         # VCVT float -> integer (result in an S register)
            sd = (vd << 1) | D_
            conv = 'f2s32' if opc2 & 1 else 'f2u32'
            if bits(w, 7, 7):                                        # round towards zero
                return 'c->v.s[%d] = %s(%s);' % (sd, conv, Dm)
            return 'c->v.s[%d] = %s(nearbyint(%s));' % (sd, conv, Dm)
        if opc2 in (2, 3):
            raise Unimpl('vcvt-half')
        if opc2 in (10, 11, 14, 15):
            raise Unimpl('vcvt-fixed')
        if opc2 in (6, 7):
            raise Unimpl('vrint')
        raise Unimpl('vfp-other')


# ----------------------------------------------------------------------------- whole module
class Env:
    def __init__(self, mod, ida):
        self.mod = mod
        self.data = set()
        for s, e in ida['text_data']:
            for a in range(s & ~3, e, 4):
                self.data.add(a)
        self.funcs = {}                      # entry -> sorted instruction addresses
        self.owner = {}                      # instruction address -> function entry
        for f in ida['functions']:
            addrs = []
            for s, e in sorted(f['chunks']):
                addrs.extend(a for a in range(s, e, 4) if a not in self.data)
            self.funcs[f['ea']] = addrs
            for a in addrs:
                self.owner.setdefault(a, f['ea'])
        self.plt = {}
        for ea in list(self.funcs):
            sym = mod.plt_target(ea)
            if sym is not None:
                self.plt[ea] = sym
        self.imports = set()
        self.entries = set(self.funcs)       # every address that becomes a C function
        self.extra = set()                   # entries that are not IDA function starts (secondary entries)

    def import_name(self, sym):
        return 'imp_' + mangle(self.mod.syms[sym][0])

    def call(self, target):
        if target in self.plt:
            self.imports.add(self.plt[target])
            return '%s(c);' % self.import_name(self.plt[target])
        if target not in self.funcs:
            self.extra.add(target)
        return 'f_%x(c);' % target


def lift_function(env, entry, stats=None):
    """C source of the function entered at `entry` (an IDA function start or an address inside one)."""
    owner = env.owner.get(entry)
    if entry in env.funcs:
        addrs = env.funcs[entry]
    elif owner is not None:
        addrs = env.funcs[owner]
    else:
        return None
    fn = Fn(env, entry, set(addrs))
    lines = []
    prev = None

    def fallthrough(after):
        """Statement for running past the end of a chunk, unless its last instruction cannot fall through."""
        last = lines[-1][1]
        if last == 'return;' or last.startswith(('goto ', '{ jt', 'c->r[13] += 4; return;')) or \
                (last.startswith('{') and last.endswith(('return; }', 'goto L_dispatch; }'))):
            return None
        if after in fn.addrs:
            return 'goto L_%x;' % after
        if after in env.funcs or after in env.plt:
            return fn.goto(after)
        return 'guest_trap("ran past the end of a function", %s); return;' % u32(after)

    for pc in addrs:
        if prev is not None and pc != prev + 4:
            s = fallthrough(prev + 4)
            if s:
                lines.append((None, s))
        w = env.mod.word(pc)
        try:
            stmt = fn.lift(w, pc)
            if stats is not None:
                stats['ok'] += 1
        except Unimpl as e:
            stmt = 'guest_unimpl(%s, 0x%08xu, "%s");' % (u32(pc), w, e)
            if stats is not None:
                stats['bad'][str(e)] += 1
                if str(e) not in stats['example']:
                    stats['example'][str(e)] = (pc, w)
        lines.append((pc, stmt))
        prev = pc
    if not lines:
        return None
    out = ['void f_%x(Ctx *c) {' % entry, '  c->fn = %s;' % u32(entry)]
    if fn.has_dispatch:
        out.append('  uint32_t jt;')
    if addrs[0] != entry:
        out.append('  goto L_%x;' % entry)
    for pc, stmt in lines:
        out.append(('L_%x: %s' % (pc, stmt)) if pc is not None else '  ' + stmt)
    s = fallthrough(addrs[-1] + 4)
    if s:
        out.append('  ' + s)
    if fn.has_dispatch:
        out.append('L_dispatch: switch (jt) {')
        out.extend('  case %s: goto L_%x;' % (u32(pc), pc) for pc in addrs)
        out.append('  default: call_indirect(c, jt); return; }')
    out.append('}')
    return '\n'.join(out)


def cmd_cover(elf, ida_json):
    mod = Module(elf)
    ida = json.load(open(ida_json))
    env = Env(mod, ida)
    stats = {'ok': 0, 'bad': collections.Counter(), 'example': {}}
    n = 0
    for entry in sorted(env.funcs):
        if entry in env.plt:
            continue
        lift_function(env, entry, stats)
        n += 1
    total = stats['ok'] + sum(stats['bad'].values())
    print('functions %d (+%d PLT stubs)   instructions %d   translated %d (%.2f%%)   secondary entries %d   imports used %d' % (
        n, len(env.plt), total, stats['ok'], 100.0 * stats['ok'] / max(total, 1), len(env.extra), len(env.imports)))
    for r, k in stats['bad'].most_common(40):
        pc, w = stats['example'][r]
        print('  %-18s %7d   e.g. %x: %08x' % (r, k, pc, w))


def cmd_lift(elf, ida_json, outdir, only=None, per_file=20000):
    mod = Module(elf)
    ida = json.load(open(ida_json))
    env = Env(mod, ida)
    os.makedirs(outdir, exist_ok=True)
    todo = sorted(only) if only else sorted(e for e in env.funcs if e not in env.plt)
    done, files, buf, count = set(), [], [], 0
    stats = {'ok': 0, 'bad': collections.Counter(), 'example': {}}

    def flush():
        nonlocal buf, count
        if not buf:
            return
        path = os.path.join(outdir, 'c_%04d.c' % len(files))
        with open(path, 'w') as f:
            f.write('/* generated by a32lift.py -- do not edit */\n#include "a32rt.h"\n#include "funcs.h"\n\n')
            f.write('\n\n'.join(buf) + '\n')
        files.append(path)
        buf, count = [], 0

    while todo:
        for entry in todo:
            if entry in done:
                continue
            done.add(entry)
            src = lift_function(env, entry, stats)
            if src is None:
                src = 'void f_%x(Ctx *c) { guest_trap("call to an address outside every function", %s); }' % (entry, u32(entry))
            buf.append(src)
            count += src.count('\n')
            if count >= per_file:
                flush()
        todo = sorted(e for e in env.extra if e not in done) if not only else []
    flush()
    entries = sorted(done)
    with open(os.path.join(outdir, 'funcs.h'), 'w') as f:
        f.write('/* generated by a32lift.py */\n#pragma once\n')
        for sym in sorted(env.imports, key=lambda s: env.import_name(s)):
            f.write('void %s(Ctx *c);\n' % env.import_name(sym))
        for e in entries:
            f.write('void f_%x(Ctx *c);\n' % e)
    with open(os.path.join(outdir, 'table.c'), 'w') as f:
        f.write('/* generated by a32lift.py: sorted entry table for call_indirect */\n#include "a32rt.h"\n#include "funcs.h"\n')
        f.write('typedef struct { uint32_t addr; GuestFn fn; } FnEntry;\nconst FnEntry fn_table[] = {\n')
        f.writelines('  {%s, f_%x},\n' % (u32(e), e) for e in entries)
        f.write('};\nconst int fn_table_count = %d;\n' % len(entries))
        f.write('const struct { uint32_t addr; GuestFn fn; const char *name; } plt_table[] = {\n')
        for ea, sym in sorted(env.plt.items()):
            if sym in env.imports:
                f.write('  {%s, %s, "%s"},\n' % (u32(ea), env.import_name(sym), mod.syms[sym][0]))
        f.write('  {0, 0, 0}\n};\n')
    total = stats['ok'] + sum(stats['bad'].values())
    print('wrote %d files, %d functions, %d instructions (%d not translated), %d imports' % (
        len(files), len(entries), total, total - stats['ok'], len(env.imports)))


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'cover':
        cmd_cover(sys.argv[2], sys.argv[3])
    elif cmd == 'lift':
        only = None
        if '--only' in sys.argv:
            only = {int(x, 16) for x in sys.argv[sys.argv.index('--only') + 1].split(',')}
        cmd_lift(sys.argv[2], sys.argv[3], sys.argv[4], only)
    else:
        raise SystemExit(__doc__)
