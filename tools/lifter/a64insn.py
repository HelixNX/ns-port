"""AArch64 instruction semantics as C statements: integer data processing, branches, loads and stores, system.
FP and SIMD are in a64simd.py. Every function takes the Fn being lifted (a64lift.Fn), the instruction word and
its absolute address, and returns one C statement (possibly a { block }); it raises Unimpl for anything not handled.
"""


class Unimpl(Exception):
    pass


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def mask(n):
    return (1 << n) - 1


def hx(v, sf=True):
    """C literal for an unsigned constant of the operation width."""
    v &= mask(64 if sf else 32)
    if sf:
        return '0x%xull' % v
    return '0x%xu' % v


# ----------------------------------------------------------------------------- conditions
COND_NAMES = ['eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al', 'nv']


def cond_flags(cc):
    """Condition from the flag locals."""
    base = ['ZF', 'CF', 'NF', 'VF', '(CF && !ZF)', '(NF == VF)', '(!ZF && NF == VF)', '1'][cc >> 1]
    if cc >= 14:
        return '1'
    return '!' + base if cc & 1 else base


def cond(fn, cc):
    """C condition for condition code cc. When the flags were last set by a compare in the same basic block, the
    condition is written on the compared values (FA, FB) so the compiler can fold it into one host compare; the
    flag locals are still kept exact for any later use."""
    if cc >= 14:
        return '1'
    fn.use_flags()
    fk = fn.fk
    if fk is None:
        return cond_flags(cc)
    kind, sf = fk
    if kind == 'fcmp':                      # FFA, FFB: the compared values (doubles); unordered as on the console
        tab = {0: 'FFA == FFB', 1: '!(FFA == FFB)', 2: '!(FFA < FFB)', 3: 'FFA < FFB', 4: 'FFA < FFB',
               5: '!(FFA < FFB)', 8: '!(FFA <= FFB)', 9: 'FFA <= FFB', 10: 'FFA >= FFB', 11: '!(FFA >= FFB)',
               12: 'FFA > FFB', 13: '!(FFA > FFB)'}
        return '(%s)' % tab[cc] if cc in tab else cond_flags(cc)
    u, s = ('u64', 's64') if sf else ('u32', 's32')
    a, b = '(%s)FA' % u, '(%s)FB' % u
    sa, sb = '(%s)FA' % s, '(%s)FB' % s
    if kind == 'sub':
        tab = {0: '%s == %s' % (a, b), 1: '%s != %s' % (a, b), 2: '%s >= %s' % (a, b), 3: '%s < %s' % (a, b),
               8: '%s > %s' % (a, b), 9: '%s <= %s' % (a, b), 10: '%s >= %s' % (sa, sb), 11: '%s < %s' % (sa, sb),
               12: '%s > %s' % (sa, sb), 13: '%s <= %s' % (sa, sb)}
    elif kind == 'add':
        tab = {0: '(%s)(FA + FB) == 0' % u, 1: '(%s)(FA + FB) != 0' % u}
    else:                                   # logical: result in FA, C = V = 0
        tab = {0: '%s == 0' % a, 1: '%s != 0' % a, 2: '0', 3: '1', 4: '%s < 0' % sa, 5: '%s >= 0' % sa,
               6: '0', 7: '1', 8: '0', 9: '1', 10: '%s >= 0' % sa, 11: '%s < 0' % sa, 12: '%s > 0' % sa,
               13: '%s <= 0' % sa}
    if cc in tab:
        return '(%s)' % tab[cc]
    return cond_flags(cc)


def set_flags_sub(fn, sf, a, b, carry_in=None):
    """Statements computing a - b (+ flags) into temporary r; returns (stmts). a, b are C expressions."""
    fn.use_flags()
    u, top = ('u64', 63) if sf else ('u32', 31)
    if carry_in is None:
        fn.temp('FA'), fn.temp('FB')
        s = '%s a = %s, b = %s, r = a - b; NF = r >> %d; ZF = r == 0; CF = a >= b; VF = ((a ^ b) & (a ^ r)) >> %d; ' \
            'FA = a; FB = b;' % (u, a, b, top, top)
        fn.fk = ('sub', sf)
    else:                                   # SBCS: a + ~b + C
        s = '%s a = %s, b = ~(%s), ci = CF, r = a + b + ci; NF = r >> %d; ZF = r == 0; CF = ci ? r <= a : r < a; ' \
            'VF = (~(a ^ b) & (a ^ r)) >> %d;' % (u, a, b, top, top)
        fn.fk = None
    return s


def set_flags_add(fn, sf, a, b, carry_in=None):
    fn.use_flags()
    u, top = ('u64', 63) if sf else ('u32', 31)
    if carry_in is None:
        fn.temp('FA'), fn.temp('FB')
        s = '%s a = %s, b = %s, r = a + b; NF = r >> %d; ZF = r == 0; CF = r < a; VF = (~(a ^ b) & (a ^ r)) >> %d; ' \
            'FA = a; FB = b;' % (u, a, b, top, top)
        fn.fk = ('add', sf)
    else:
        s = '%s a = %s, b = %s, ci = CF, r = a + b + ci; NF = r >> %d; ZF = r == 0; CF = ci ? r <= a : r < a; ' \
            'VF = (~(a ^ b) & (a ^ r)) >> %d;' % (u, a, b, top, top)
        fn.fk = None
    return s


def set_flags_logic(fn, sf, expr):
    fn.use_flags()
    fn.temp('FA')
    u, top = ('u64', 63) if sf else ('u32', 31)
    fn.fk = ('log', sf)
    return '%s r = %s; NF = r >> %d; ZF = r == 0; CF = 0; VF = 0; FA = r;' % (u, expr, top)


# ----------------------------------------------------------------------------- operands
def decode_bitmask(n, imms, immr, sf):
    v = (n << 6) | (~imms & 0x3f)
    if v == 0:
        raise Unimpl('bitmask-reserved')
    length = v.bit_length() - 1
    levels = mask(length)
    s, r = imms & levels, immr & levels
    if s == levels:
        raise Unimpl('bitmask-reserved')
    esize = 1 << length
    welem = mask(s + 1)
    elem = ((welem >> r) | (welem << (esize - r))) & mask(esize) if r else welem
    size = 64 if sf else 32
    out = 0
    for i in range(0, size, esize):
        out |= elem << i
    return out


def shifted(fn, sf, rm, typ, amount):
    """Shifted-register operand (LSL, LSR, ASR, ROR)."""
    size = 64 if sf else 32
    v = fn.r(rm, sf)
    if amount == 0:
        return v
    if typ == 0:
        return '(%s << %d)' % (v, amount)
    if typ == 1:
        return '(%s >> %d)' % (v, amount)
    if typ == 2:
        return '(%s)((%s)%s >> %d)' % ('u64' if sf else 'u32', 's64' if sf else 's32', v, amount)
    return '(%s >> %d | %s << %d)' % (v, amount, v, size - amount)


def extended(fn, sf, rm, option, amount):
    """Extended-register operand: UXTB/H/W/X, SXTB/H/W/X then LSL #amount (0..4), as a value of width sf."""
    src = {0: '(u8)%s', 1: '(u16)%s', 2: '(u32)%s', 3: '%s', 4: '(s8)%s', 5: '(s16)%s', 6: '(s32)%s', 7: '%s'}[option]
    v = src % fn.x(rm)
    t = 'u64' if sf else 'u32'
    e = '(%s)%s' % (t, v) if option not in (3, 7) or not sf else v
    if amount:
        e = '(%s << %d)' % (e, amount)
    return e


# ----------------------------------------------------------------------------- top level
def branch_targets(w, pc):
    """Direct branch targets of an instruction (for labels)."""
    if w & 0x7c000000 == 0x14000000:                                  # B, BL
        if w & 0x80000000:
            return ()
        return (pc + (sext(bits(w, 25, 0), 26) << 2),)
    if w & 0xff000010 == 0x54000000:                                  # B.cond
        return (pc + (sext(bits(w, 23, 5), 19) << 2),)
    if w & 0x7e000000 == 0x34000000:                                  # CBZ, CBNZ
        return (pc + (sext(bits(w, 23, 5), 19) << 2),)
    if w & 0x7e000000 == 0x36000000:                                  # TBZ, TBNZ
        return (pc + (sext(bits(w, 18, 5), 14) << 2),)
    return ()


def lift_insn(fn, w, pc):
    op0 = bits(w, 28, 25)
    if op0 in (8, 9):
        return dp_imm(fn, w, pc)
    if op0 in (10, 11):
        return branch_sys(fn, w, pc)
    if op0 & 5 == 4:
        return loadstore(fn, w, pc)
    if op0 & 7 == 5:
        return dp_reg(fn, w, pc)
    if op0 & 7 == 7:
        import a64simd
        return a64simd.lift(fn, w, pc)
    if w & 0xffff0000 == 0 or w == 0xe7ffdefe:                        # UDF, and the A32 trap word used as padding
        return '{ SAVE; guest_trap(c, "udf", 0x%xull); return; }' % pc
    raise Unimpl('unallocated')


# ----------------------------------------------------------------------------- data processing (immediate)
def dp_imm(fn, w, pc):
    sf = bits(w, 31, 31)
    rd, rn = bits(w, 4, 0), bits(w, 9, 5)
    op = bits(w, 25, 23)
    if op in (0, 1):                                                  # ADR, ADRP
        imm = sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
        if w & 0x80000000:
            return fn.setx(rd, hx((pc & ~0xfff) + (imm << 12)))
        return fn.setx(rd, hx(pc + imm))
    if op == 2:                                                       # ADD/SUB (immediate)
        sub, s = bits(w, 30, 30), bits(w, 29, 29)
        imm = bits(w, 21, 10) << (12 if bits(w, 22, 22) else 0)
        if bits(w, 22, 22) > 1:
            raise Unimpl('addsub-imm-shift')
        a = fn.r(rn, sf, sp=True)
        if not s:
            if imm == 0:
                return fn.setr(rd, sf, a, sp=True)
            return fn.setr(rd, sf, '%s %s %s' % (a, '-' if sub else '+', hx(imm, sf)), sp=True)
        f = set_flags_sub if sub else set_flags_add
        return '{ %s %s }' % (f(fn, sf, a, hx(imm, sf)), fn.setr(rd, sf, 'r'))
    if op == 4:                                                       # logical (immediate)
        opc = bits(w, 30, 29)
        if not sf and bits(w, 22, 22):
            raise Unimpl('logimm-n')
        imm = decode_bitmask(bits(w, 22, 22), bits(w, 15, 10), bits(w, 21, 16), sf)
        a = fn.r(rn, sf)
        k = hx(imm, sf)
        if opc == 3:
            return '{ %s %s }' % (set_flags_logic(fn, sf, '%s & %s' % (a, k)), fn.setr(rd, sf, 'r'))
        e = {0: '%s & %s', 1: '%s | %s', 2: '%s ^ %s'}[opc] % (a, k)
        if opc == 1 and rn == 31:
            e = k
        return fn.setr(rd, sf, e, sp=True)
    if op == 5:                                                       # MOVN, MOVZ, MOVK
        opc, hw, imm = bits(w, 30, 29), bits(w, 22, 21), bits(w, 20, 5)
        if opc == 1 or (not sf and hw > 1):
            raise Unimpl('movw')
        sh = hw * 16
        if opc == 0:
            return fn.setr(rd, sf, hx(~(imm << sh), sf))
        if opc == 2:
            return fn.setr(rd, sf, hx(imm << sh, sf))
        return fn.setr(rd, sf, '(%s & %s) | %s' % (fn.r(rd, sf), hx(~(0xffff << sh), sf), hx(imm << sh, sf)))
    if op == 6:                                                       # SBFM, BFM, UBFM
        return bitfield(fn, w, sf, rd, rn)
    if op == 7:                                                       # EXTR
        if bits(w, 30, 29) or bits(w, 21, 21) or bits(w, 22, 22) != sf:
            raise Unimpl('extr')
        lsb = bits(w, 15, 10)
        size = 64 if sf else 32
        rm = bits(w, 20, 16)
        if lsb >= size:
            raise Unimpl('extr')
        if lsb == 0:
            return fn.setr(rd, sf, fn.r(rm, sf))
        return fn.setr(rd, sf, '(%s >> %d) | (%s << %d)' % (fn.r(rm, sf), lsb, fn.r(rn, sf), size - lsb))
    raise Unimpl('dp-imm')


def bitfield(fn, w, sf, rd, rn):
    opc, n, immr, imms = bits(w, 30, 29), bits(w, 22, 22), bits(w, 21, 16), bits(w, 15, 10)
    size = 64 if sf else 32
    if n != sf or immr >= size or imms >= size or opc == 3:
        raise Unimpl('bitfield')
    u, s = ('u64', 's64') if sf else ('u32', 's32')
    src = fn.r(rn, sf)
    if opc == 2:                                                      # UBFM: LSR / UBFX / LSL / UBFIZ / UXTB...
        if imms >= immr:
            width = imms - immr + 1
            e = '%s >> %d' % (src, immr) if immr else src
            if imms != size - 1:
                e = '(%s) & %s' % (e, hx(mask(width), sf))
            return fn.setr(rd, sf, e)
        width = imms + 1
        return fn.setr(rd, sf, '(%s & %s) << %d' % (src, hx(mask(width), sf), size - immr))
    if opc == 0:                                                      # SBFM: ASR / SBFX / SBFIZ / SXTB...
        up = size - 1 - imms
        if imms >= immr:
            e = '(%s)((%s)(%s << %d) >> %d)' % (u, s, src, up, up + immr) if up else '(%s)((%s)%s >> %d)' % (u, s, src, immr)
            return fn.setr(rd, sf, e)
        return fn.setr(rd, sf, '(%s)((%s)(%s << %d) >> %d) << %d' % (u, s, src, up, up, size - immr))
    dst = fn.r(rd, sf)                                                # BFM: BFXIL / BFI
    if imms >= immr:
        width = imms - immr + 1
        m = hx(mask(width), sf)
        return fn.setr(rd, sf, '(%s & ~%s) | ((%s >> %d) & %s)' % (dst, m, src, immr, m))
    width, lsb = imms + 1, size - immr
    m = hx(mask(width) << lsb, sf)
    return fn.setr(rd, sf, '(%s & ~%s) | ((%s << %d) & %s)' % (dst, m, src, lsb, m))


# ----------------------------------------------------------------------------- branches, exceptions, system
SYSREGS = {                       # (op0, op1, CRn, CRm, op2) -> name
    (3, 3, 13, 0, 2): 'tpidr', (3, 3, 13, 0, 3): 'tpidrro', (3, 3, 14, 0, 1): 'cntpct', (3, 3, 14, 0, 2): 'cntvct',
    (3, 3, 14, 0, 0): 'cntfrq', (3, 3, 4, 4, 0): 'fpcr', (3, 3, 4, 4, 1): 'fpsr', (3, 3, 4, 2, 0): 'nzcv',
    (3, 3, 0, 0, 7): 'dczid', (3, 3, 0, 0, 1): 'ctr',
}


def branch_sys(fn, w, pc):
    if w & 0x7c000000 == 0x14000000:                                  # B, BL
        t = pc + (sext(bits(w, 25, 0), 26) << 2)
        if w & 0x80000000:
            return fn.call(t, pc + 4)
        return fn.jump(t)
    if w & 0xff000010 == 0x54000000:                                  # B.cond
        t = pc + (sext(bits(w, 23, 5), 19) << 2)
        cc = bits(w, 3, 0)
        if cc >= 14:
            return fn.jump(t)
        return 'if %s %s' % (paren(cond(fn, cc)), fn.jump(t))
    if w & 0x7e000000 == 0x34000000:                                  # CBZ, CBNZ
        sf, t = bits(w, 31, 31), pc + (sext(bits(w, 23, 5), 19) << 2)
        return 'if (%s %s 0) %s' % (fn.r(bits(w, 4, 0), sf), '!=' if bits(w, 24, 24) else '==', fn.jump(t))
    if w & 0x7e000000 == 0x36000000:                                  # TBZ, TBNZ
        b = (bits(w, 31, 31) << 5) | bits(w, 23, 19)
        t = pc + (sext(bits(w, 18, 5), 14) << 2)
        return 'if (%s(%s >> %d & 1)) %s' % ('' if bits(w, 24, 24) else '!', fn.x(bits(w, 4, 0)), b, fn.jump(t))
    if w & 0xfffffc1f == 0xd61f0000:                                  # BR
        return fn.jump_ind(fn.x(bits(w, 9, 5)), pc)
    if w & 0xfffffc1f == 0xd63f0000:                                  # BLR
        return fn.call_ind(fn.x(bits(w, 9, 5)), pc + 4)
    if w & 0xfffffc1f == 0xd65f0000:                                  # RET
        return fn.ret(fn.x(bits(w, 9, 5)))
    if w & 0xffe0001f == 0xd4000001:                                  # SVC
        return fn.svc(bits(w, 20, 5))
    if w & 0xffe0001f == 0xd4200000:                                  # BRK
        return '{ SAVE; guest_trap(c, "brk", 0x%xull); return; }' % pc
    if w & 0xfffff01f == 0xd503201f:                                  # hints
        op = bits(w, 11, 5)
        if op == 1:
            return 'host_yield();'
        return ';'
    if w & 0xfffff0ff == 0xd503305f:                                  # CLREX
        return 'c->ex_addr = ~0ull;'
    if w & 0xfffff09f == 0xd503309f:                                  # DSB, DMB
        return 'FENCE();'
    if w & 0xfffff0ff == 0xd50330df:                                  # ISB
        return ';'
    if w & 0xfff00000 in (0xd5300000, 0xd5100000):                   # MRS, MSR (register)
        key = (2 + bits(w, 19, 19), bits(w, 18, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 7, 5))
        name = SYSREGS.get(key)
        rt = bits(w, 4, 0)
        if name is None:
            raise Unimpl('sysreg-%d-%d-%d-%d-%d' % key)
        if w & 0x00200000:                                            # MRS
            if name == 'cntpct' or name == 'cntvct':
                return fn.setx(rt, 'host_cntpct()')
            if name == 'cntfrq':
                return fn.setx(rt, '19200000ull')
            if name == 'dczid':
                return fn.setx(rt, '4ull')
            if name == 'ctr':
                return fn.setx(rt, '0x8444c004ull')
            if name == 'nzcv':
                fn.use_flags()
                return fn.setx(rt, '((u64)NF << 31) | ((u64)ZF << 30) | ((u64)CF << 29) | ((u64)VF << 28)')
            return fn.setx(rt, 'c->%s' % name)
        if name == 'nzcv':
            fn.use_flags()
            fn.fk = None
            v = fn.x(rt)
            return 'NF = %s >> 31 & 1; ZF = %s >> 30 & 1; CF = %s >> 29 & 1; VF = %s >> 28 & 1;' % (v, v, v, v)
        if name in ('tpidr', 'fpcr', 'fpsr'):
            return 'c->%s = %s;' % (name, fn.x(rt)) + (' host_set_fpcr(c);' if name == 'fpcr' else '')
        raise Unimpl('msr-' + name)
    if w & 0xfff80000 == 0xd5080000:                                  # SYS: DC, IC, ...
        op1, crn, crm, op2 = bits(w, 18, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 7, 5)
        if (op1, crn, crm, op2) == (3, 7, 4, 1):                    # DC ZVA
            return 'memset((void *)(%s & ~63ull), 0, 64);' % fn.x(bits(w, 4, 0))
        if crn == 7:                                                  # cache maintenance: nothing to do
            fn.x(bits(w, 4, 0))
            return ';'
        raise Unimpl('sys')
    if w & 0xfff8f01f == 0xd500401f:                                  # MSR (immediate): PSTATE fields
        raise Unimpl('msr-imm')
    raise Unimpl('branch-sys')


def paren(e):
    return e if e.startswith('(') and e.endswith(')') and balanced(e[1:-1]) else '(%s)' % e


def balanced(s):
    d = 0
    for ch in s:
        d += ch == '('
        d -= ch == ')'
        if d < 0:
            return False
    return d == 0


# ----------------------------------------------------------------------------- loads and stores
LD = {0: 'LD8', 1: 'LD16', 2: 'LD32', 3: 'LD64'}
ST = {0: 'ST8', 1: 'ST16', 2: 'ST32', 3: 'ST64'}
SEXT = {0: 's8', 1: 's16', 2: 's32'}


def loadstore(fn, w, pc):
    if bits(w, 29, 24) == 0b001000:
        return exclusive(fn, w, pc)
    if bits(w, 29, 27) == 0b011 and bits(w, 25, 24) == 0:
        return load_literal(fn, w, pc)
    if bits(w, 29, 27) == 0b101:
        return load_pair(fn, w, pc)
    if bits(w, 29, 27) == 0b111:
        return load_reg(fn, w, pc)
    if bits(w, 31, 31) == 0 and bits(w, 29, 24) in (0b001100, 0b001101):
        import a64simd
        return a64simd.ldst_struct(fn, w, pc)
    raise Unimpl('ldst')


def mem_read(fn, size, v, signed_to=None):
    """Expression loading `size` (0..3 = 8..64 bits) at address expression v."""
    e = '%s(%s)' % (LD[size], v)
    if signed_to is not None:
        e = '(%s)(%s)%s' % ('u64' if signed_to else 'u32', SEXT[size], e)
    return e


def vreg_load(fn, rt, sz, addr):
    """SIMD&FP scalar load of 2**sz bytes (0..4) into Vrt, zeroing the rest."""
    if sz == 4:
        return fn.setv(rt, 'LDQ(%s)' % addr)
    return fn.setv(rt, 'vz%d(%s(%s))' % (8 << sz, LD[sz], addr))


def vreg_store(fn, rt, sz, addr):
    if sz == 4:
        return 'STQ(%s, %s);' % (addr, fn.v(rt))
    return '%s(%s, (u%d)%s[0]);' % (ST[sz], addr, 8 << sz, '((v%du%d)%s)' % (128 // (8 << sz), 8 << sz, fn.v(rt)))


def load_literal(fn, w, pc):
    opc, vbit, rt = bits(w, 31, 30), bits(w, 26, 26), bits(w, 4, 0)
    addr = pc + (sext(bits(w, 23, 5), 19) << 2)
    mod = fn.mod
    off = addr - mod.base
    const = mod.m.text[0] <= off < mod.m.ro[1]                      # text and rodata never change
    if vbit:
        sz = {0: 2, 1: 3, 2: 4}.get(opc)
        if sz is None:
            raise Unimpl('ldr-lit-v')
        if const:
            raw = mod.m.bytes(off, 1 << sz)
            if sz == 4:
                lo, hi = int.from_bytes(raw[:8], 'little'), int.from_bytes(raw[8:], 'little')
                return fn.setv(rt, '(v128){0x%xull, 0x%xull}' % (lo, hi))
            return fn.setv(rt, 'vz%d(0x%xu%s)' % (8 << sz, int.from_bytes(raw, 'little'), 'll' if sz == 3 else ''))
        return vreg_load(fn, rt, sz, hx(addr))
    if opc == 3:
        return ';'                                                    # PRFM
    if const:
        if opc == 0:
            return fn.setw(rt, hx(mod.m.u32(off), False))
        if opc == 1:
            return fn.setx(rt, hx(mod.m.u64(off)))
        return fn.setx(rt, hx(sext(mod.m.u32(off), 32)))
    if opc == 0:
        return fn.setw(rt, mem_read(fn, 2, hx(addr)))
    if opc == 1:
        return fn.setx(rt, mem_read(fn, 3, hx(addr)))
    return fn.setx(rt, mem_read(fn, 2, hx(addr), signed_to=1))


def load_pair(fn, w, pc):
    opc, vbit, mode, l = bits(w, 31, 30), bits(w, 26, 26), bits(w, 24, 23), bits(w, 22, 22)
    rt, rt2, rn = bits(w, 4, 0), bits(w, 14, 10), bits(w, 9, 5)
    if vbit:
        if opc == 3:
            raise Unimpl('ldp-v')
        sz = 2 + opc
    else:
        if opc == 3 or (opc == 1 and not l):
            raise Unimpl('ldp-opc')
        sz = 3 if opc == 2 else 2
    scale = 1 << sz
    imm = sext(bits(w, 21, 15), 7) * scale
    base = fn.x(rn, sp=True)
    if mode == 1:                                                     # post-index
        a = base
    else:
        a = '%s + %s' % (base, hx(imm)) if imm else base
    parts = ['u64 a = %s;' % a]
    if vbit:
        if l:
            parts.append(vreg_load(fn, rt, sz, 'a'))
            parts.append(vreg_load(fn, rt2, sz, 'a + %d' % scale))
        else:
            parts.append(vreg_store(fn, rt, sz, 'a'))
            parts.append(vreg_store(fn, rt2, sz, 'a + %d' % scale))
    elif l:
        if opc == 1:                                                  # LDPSW
            v1, v2 = mem_read(fn, 2, 'a', 1), mem_read(fn, 2, 'a + 4', 1)
            parts.append('u64 t1 = %s, t2 = %s;' % (v1, v2))
            parts.append(fn.setx(rt, 't1'))
            parts.append(fn.setx(rt2, 't2'))
        else:
            parts.append('u64 t1 = %s, t2 = %s;' % (mem_read(fn, sz, 'a'), mem_read(fn, sz, 'a + %d' % scale)))
            parts.append(fn.setr(rt, sz == 3, 't1'))
            parts.append(fn.setr(rt2, sz == 3, 't2'))
    else:
        parts.append('%s(a, %s);' % (ST[sz], fn.r(rt, sz == 3)))
        parts.append('%s(a + %d, %s);' % (ST[sz], scale, fn.r(rt2, sz == 3)))
    if mode == 1:
        parts.append(fn.setx(rn, 'a + %s' % hx(imm), sp=True))
    elif mode == 3:
        parts.append(fn.setx(rn, 'a', sp=True))
    return '{ ' + ' '.join(parts) + ' }'


def load_reg(fn, w, pc):
    size, vbit, opc = bits(w, 31, 30), bits(w, 26, 26), bits(w, 23, 22)
    rt, rn = bits(w, 4, 0), bits(w, 9, 5)
    base = fn.x(rn, sp=True)
    wb = None                                                         # (writeback, post)
    if bits(w, 25, 24) == 1:                                          # unsigned offset
        if vbit:
            sz = size + 4 * (opc >> 1)
        else:
            sz = size
        imm = bits(w, 21, 10) << sz
        a = '%s + %s' % (base, hx(imm)) if imm else base
    elif bits(w, 21, 21) == 0:
        imm = sext(bits(w, 20, 12), 9)
        kind = bits(w, 11, 10)
        if kind == 2:
            raise Unimpl('ldtr')                                      # unprivileged
        if kind == 0:
            a = '%s + %s' % (base, hx(imm)) if imm else base
        elif kind == 1:
            a, wb = base, ('post', imm)
        else:
            a, wb = '%s + %s' % (base, hx(imm)), ('pre', imm)
    elif bits(w, 11, 10) == 2:                                        # register offset
        rm, option, s = bits(w, 20, 16), bits(w, 15, 13), bits(w, 12, 12)
        if option not in (2, 3, 6, 7):
            raise Unimpl('ldr-reg-option')
        sz = size + 4 * (opc >> 1) if vbit else size
        off = extended(fn, 1, rm, option, sz if s else 0)
        a = '%s + %s' % (base, off)
    else:
        raise Unimpl('atomic-lse')
    stmts = []
    if vbit:
        sz = size + 4 * (opc >> 1)
        if sz > 4:
            raise Unimpl('ldr-v-size')
        if wb is not None or 'u64 a' in a:
            stmts.append('u64 a = %s;' % a)
            a = 'a'
        if opc & 1:
            stmts.append(vreg_load(fn, rt, sz, a))
        else:
            stmts.append(vreg_store(fn, rt, sz, a))
    else:
        if wb is not None:
            stmts.append('u64 a = %s;' % a)
            a = 'a'
        if opc == 0:
            stmts.append('%s(%s, %s);' % (ST[size], a, fn.r(rt, size == 3)))
        elif opc == 1:
            stmts.append(fn.setr(rt, size == 3, mem_read(fn, size, a)))
        elif size == 3:
            stmts.append(';')                                         # PRFM / PRFUM
            if wb is not None:
                raise Unimpl('prfm-wb')
        elif opc == 2:                                                # sign-extend to 64
            stmts.append(fn.setx(rt, mem_read(fn, size, a, signed_to=1)))
        else:
            if size == 2:
                raise Unimpl('ldr-opc')
            stmts.append(fn.setw(rt, mem_read(fn, size, a, signed_to=0)))
    if wb is not None:
        kind, imm = wb
        stmts.append(fn.setx(rn, ('a + %s' % hx(imm)) if kind == 'post' else 'a', sp=True))
    if len(stmts) == 1:
        return stmts[0]
    return '{ ' + ' '.join(stmts) + ' }'


def exclusive(fn, w, pc):
    size, o2, l, o1, o0 = bits(w, 31, 30), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21), bits(w, 15, 15)
    rs, rt2, rn, rt = bits(w, 20, 16), bits(w, 14, 10), bits(w, 9, 5), bits(w, 4, 0)
    a = fn.x(rn, sp=True)
    sf = size == 3
    if o2:                                                            # LDAR / STLR (and the LORegion forms)
        if o1:
            raise Unimpl('cas')
        if l:
            return fn.setr(rt, sf, 'LDA%d(%s)' % (8 << size, a))
        return 'STL%d(%s, %s);' % (8 << size, a, fn.r(rt, sf))
    if o1:                                                            # pairs
        if size < 2:
            raise Unimpl('ldxp-size')
        bitsz = 8 << size
        if l:
            return '{ u64 a = %s; c->ex_addr = a; c->ex_size = %d; c->ex_val[0] = LD%d(a); c->ex_val[1] = LD%d(a + %d); ' \
                   '%s %s }' % (a, 2 * bitsz // 8, bitsz, bitsz, bitsz // 8,
                                fn.setr(rt, sf, 'c->ex_val[0]'), fn.setr(rt2, sf, 'c->ex_val[1]'))
        return fn.setw(rs, 'stxp%d(c, %s, %s, %s)' % (bitsz, a, fn.r(rt, sf), fn.r(rt2, sf)))
    if l:
        return '{ u64 a = %s; c->ex_addr = a; c->ex_size = %d; c->ex_val[0] = %s(a); %s }' % (
            a, 1 << size, ('LDA%d' if o0 else 'LD%d') % (8 << size), fn.setr(rt, sf, 'c->ex_val[0]'))
    return fn.setw(rs, 'stx%d(c, %s, %s)' % (8 << size, a, fn.r(rt, sf)))


# ----------------------------------------------------------------------------- data processing (register)
def dp_reg(fn, w, pc):
    sf, rd, rn, rm = bits(w, 31, 31), bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
    t = 'u64' if sf else 'u32'
    if bits(w, 28, 24) == 0b01010:                                    # logical (shifted register)
        opc, shift, n, imm6 = bits(w, 30, 29), bits(w, 23, 22), bits(w, 21, 21), bits(w, 15, 10)
        if not sf and imm6 >= 32:
            raise Unimpl('log-shift')
        b = shifted(fn, sf, rm, shift, imm6)
        if n:
            b = '~%s' % b
        a = fn.r(rn, sf)
        if opc == 3:
            return '{ %s %s }' % (set_flags_logic(fn, sf, '%s & %s' % (a, b)), fn.setr(rd, sf, 'r'))
        if opc == 1 and rn == 31:                                     # MOV / MVN
            return fn.setr(rd, sf, b if not n else '(%s)%s' % (t, b))
        e = '%s %s %s' % (a, {0: '&', 1: '|', 2: '^'}[opc], b)
        return fn.setr(rd, sf, e)
    if bits(w, 28, 24) == 0b01011:                                    # ADD/SUB shifted or extended
        sub, s = bits(w, 30, 30), bits(w, 29, 29)
        if bits(w, 21, 21) == 0:
            shift, imm6 = bits(w, 23, 22), bits(w, 15, 10)
            if shift == 3 or (not sf and imm6 >= 32):
                raise Unimpl('addsub-shift')
            b = shifted(fn, sf, rm, shift, imm6)
            a = fn.r(rn, sf)
            sp = False
        else:
            if bits(w, 23, 22) or bits(w, 12, 10) > 4:
                raise Unimpl('addsub-ext')
            b = extended(fn, sf, rm, bits(w, 15, 13), bits(w, 12, 10))
            a = fn.r(rn, sf, sp=True)
            sp = not s
        if s:
            f = set_flags_sub if sub else set_flags_add
            return '{ %s %s }' % (f(fn, sf, a, b), fn.setr(rd, sf, 'r'))
        if sub and rn == 31 and not sp:
            return fn.setr(rd, sf, '(%s)0 - %s' % (t, b))            # NEG
        return fn.setr(rd, sf, '%s %s %s' % (a, '-' if sub else '+', b), sp=sp)
    op21 = bits(w, 28, 21)
    if op21 == 0b11010000:                                            # ADC, ADCS, SBC, SBCS
        if bits(w, 15, 10):
            raise Unimpl('adc')
        sub, s = bits(w, 30, 30), bits(w, 29, 29)
        a, b = fn.r(rn, sf), fn.r(rm, sf)
        if s:
            f = set_flags_sub if sub else set_flags_add
            return '{ %s %s }' % (f(fn, sf, a, b, carry_in=True), fn.setr(rd, sf, 'r'))
        fn.use_flags()
        if sub:
            return fn.setr(rd, sf, '%s + (%s)~%s + CF' % (a, t, b))
        return fn.setr(rd, sf, '%s + %s + CF' % (a, b))
    if op21 == 0b11010010:                                            # CCMN, CCMP
        if not bits(w, 29, 29) or bits(w, 10, 10) or bits(w, 4, 4):
            raise Unimpl('ccmp')
        sub = bits(w, 30, 30)
        cc, nzcv = bits(w, 15, 12), bits(w, 3, 0)
        b = hx(rm, sf) if bits(w, 11, 11) else fn.r(rm, sf)
        c0 = cond(fn, cc)
        f = set_flags_sub if sub else set_flags_add
        body = f(fn, sf, fn.r(rn, sf), b)
        fn.fk = None
        return 'if %s { %s } else { NF = %d; ZF = %d; CF = %d; VF = %d; }' % (
            paren(c0), body, nzcv >> 3, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1)
    if op21 == 0b11010100:                                            # CSEL, CSINC, CSINV, CSNEG
        if bits(w, 29, 29) or bits(w, 11, 11):
            raise Unimpl('csel')
        op, o2 = bits(w, 30, 30), bits(w, 10, 10)
        c0 = cond(fn, bits(w, 15, 12))
        a = fn.r(rn, sf)
        b = fn.r(rm, sf)
        e2 = {(0, 0): b, (0, 1): '%s + 1' % b, (1, 0): '~%s' % b, (1, 1): '(%s)0 - %s' % (t, b)}[(op, o2)]
        if (op, o2) != (0, 0) and rm == 31:
            e2 = {(0, 1): '1', (1, 0): hx(-1, sf), (1, 1): '0'}[(op, o2)]
        return fn.setr(rd, sf, '%s ? %s : (%s)(%s)' % (paren(c0), a, t, e2))
    if op21 == 0b11010110:
        opcode = bits(w, 15, 10)
        if bits(w, 30, 30):                                           # 1 source
            if bits(w, 20, 16) or bits(w, 29, 29):
                raise Unimpl('dp1')
            a = fn.r(rn, sf)
            n = 64 if sf else 32
            if opcode == 0:
                return fn.setr(rd, sf, 'rbit%d(%s)' % (n, a))
            if opcode == 1:
                return fn.setr(rd, sf, 'rev16_%d(%s)' % (n, a))
            if opcode == 2:
                return fn.setr(rd, sf, ('rev32_64(%s)' if sf else '__builtin_bswap32(%s)') % a)
            if opcode == 3 and sf:
                return fn.setx(rd, '__builtin_bswap64(%s)' % a)
            if opcode == 4:
                return fn.setr(rd, sf, 'clz%d(%s)' % (n, a))
            if opcode == 5:
                return fn.setr(rd, sf, 'cls%d(%s)' % (n, a))
            raise Unimpl('dp1')
        if bits(w, 29, 29):
            raise Unimpl('dp2')
        a, b = fn.r(rn, sf), fn.r(rm, sf)
        n = 64 if sf else 32
        if opcode == 2:
            return fn.setr(rd, sf, 'udiv%d(%s, %s)' % (n, a, b))
        if opcode == 3:
            return fn.setr(rd, sf, 'sdiv%d(%s, %s)' % (n, a, b))
        if opcode == 8:
            return fn.setr(rd, sf, '%s << (%s & %d)' % (a, b, n - 1))
        if opcode == 9:
            return fn.setr(rd, sf, '%s >> (%s & %d)' % (a, b, n - 1))
        if opcode == 10:
            return fn.setr(rd, sf, '(%s)((%s)%s >> (%s & %d))' % (t, 's64' if sf else 's32', a, b, n - 1))
        if opcode == 11:
            return fn.setr(rd, sf, 'ror%d(%s, %s)' % (n, a, b))
        if opcode & 0x38 == 0x10:                                     # CRC32 / CRC32C
            sz, cbit = opcode & 3, (opcode >> 2) & 1
            if (sz == 3) != bool(sf):
                raise Unimpl('crc32')
            return fn.setw(rd, 'crc32%s_%d(%s, %s)' % ('c' if cbit else '', 8 << sz, fn.w(rn), fn.r(rm, sz == 3)))
        raise Unimpl('dp2')
    if bits(w, 28, 24) == 0b11011:                                    # 3 source
        op54, op31, o0, ra = bits(w, 30, 29), bits(w, 23, 21), bits(w, 15, 15), bits(w, 14, 10)
        if op54:
            raise Unimpl('dp3')
        if op31 == 0:
            a, b, acc = fn.r(rn, sf), fn.r(rm, sf), fn.r(ra, sf)
            if ra == 31:
                return fn.setr(rd, sf, ('(%s)0 - %s * %s' % (t, a, b)) if o0 else '%s * %s' % (a, b))
            return fn.setr(rd, sf, '%s %s %s * %s' % (acc, '-' if o0 else '+', a, b))
        if not sf:
            raise Unimpl('dp3')
        if op31 in (1, 5):                                            # SMADDL/SMSUBL, UMADDL/UMSUBL
            cast = 's32' if op31 == 1 else 'u32'
            prod = '(u64)((%s)(%s)%s * (%s)(%s)%s)' % (
                's64' if op31 == 1 else 'u64', cast, fn.x(rn), 's64' if op31 == 1 else 'u64', cast, fn.x(rm))
            if ra == 31:
                return fn.setx(rd, ('(u64)0 - %s' % prod) if o0 else prod)
            return fn.setx(rd, '%s %s %s' % (fn.x(ra), '-' if o0 else '+', prod))
        if op31 == 2 and not o0:
            return fn.setx(rd, 'smulh(%s, %s)' % (fn.x(rn), fn.x(rm)))
        if op31 == 6 and not o0:
            return fn.setx(rd, 'umulh(%s, %s)' % (fn.x(rn), fn.x(rm)))
        raise Unimpl('dp3')
    raise Unimpl('dp-reg')
