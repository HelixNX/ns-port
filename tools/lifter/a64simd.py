"""AArch64 FP and Advanced SIMD semantics as C statements (see a64insn.py for the conventions).

V registers are `v128` locals (a 16-byte clang vector of two u64); other views are casts: (v4f32)V3 etc.
A scalar FP result writes the low element and zeroes the rest (vz32/vz64/vzf32/vzf64 in a64rt.h); a 64-bit
vector result (Q = 0) zeroes the upper half (vlo).
"""
import struct

from a64insn import Unimpl, bits, sext, mask, hx, cond, paren, mem_read, ST, LD


# ----------------------------------------------------------------------------- types and views
def vt(size, kind='u', q=1):
    """Vector type: element size 0..3 (8..64 bits), kind u/s/f; q=0 gives the 8-byte type."""
    lanes = (16 if q else 8) >> size
    return 'v%d%s%d' % (lanes, kind, 8 << size)


def et(size, kind='u'):
    return '%s%d' % ('f' if kind == 'f' else kind, 8 << size) if kind != 'f' else ('f32' if size == 2 else 'f64')


def view(fn, n, size, kind='u'):
    return '((%s)%s)' % (vt(size, kind), fn.v(n))


def put(fn, d, q, expr):
    """Vd = vector expression (any 16-byte vector type); upper half zeroed for Q = 0."""
    if q:
        return fn.setv(d, '(v128)(%s)' % expr)
    return fn.setv(d, 'vlo((v128)(%s))' % expr)


def fsz(ftype):
    """FP type field (bits 23:22) -> element size (2 = single, 3 = double)."""
    if ftype == 0:
        return 2
    if ftype == 1:
        return 3
    raise Unimpl('fp-half')


def fread(fn, n, size):
    """Scalar FP value of Vn (size 2 = float, 3 = double)."""
    return '((%s)%s)[0]' % ('v4f32' if size == 2 else 'v2f64', fn.v(n))


def fwrite(fn, d, size, expr):
    return fn.setv(d, '%s(%s)' % ('vzf32' if size == 2 else 'vzf64', expr))


def iread(fn, n, size):
    """Low element of Vn as an unsigned integer of 8 << size bits."""
    return '((%s)%s)[0]' % (vt(size), fn.v(n))


def iwrite(fn, d, size, expr):
    return fn.setv(d, 'vz%d(%s)' % (8 << size, expr))


def vsel(m, a, b):
    """Lane select: m is a comparison (all-ones / zero lanes); a where set, b elsewhere."""
    return '((((v128)(%s)) & (v128)(%s)) | (~((v128)(%s)) & (v128)(%s)))' % (m, a, m, b)


def splat(t, e, lanes):
    return '(%s){%s}' % (t, ', '.join([e] * lanes))


def shuffle(a, b, idx):
    return '__builtin_shufflevector(%s, %s, %s)' % (a, b, ', '.join(str(i) for i in idx))


def vfp_expand_imm(imm8, size):
    sign = imm8 >> 7
    b6 = (imm8 >> 6) & 1
    if size == 2:
        exp = ((b6 ^ 1) << 7) | ((0x1f if b6 else 0) << 2) | ((imm8 >> 4) & 3)
        return (sign << 31) | (exp << 23) | ((imm8 & 0xf) << 19)
    exp = ((b6 ^ 1) << 10) | ((0xff if b6 else 0) << 2) | ((imm8 >> 4) & 3)
    return (sign << 63) | (exp << 52) | ((imm8 & 0xf) << 48)


def f32lit(bitsv):
    f = struct.unpack('<f', struct.pack('<I', bitsv))[0]
    return 'f32_from_bits(0x%xu)' % bitsv if f != f or f in (float('inf'), float('-inf')) else repr(f) + 'f'


def f64lit(bitsv):
    f = struct.unpack('<d', struct.pack('<Q', bitsv))[0]
    return 'f64_from_bits(0x%xull)' % bitsv if f != f or f in (float('inf'), float('-inf')) else repr(f)


# ----------------------------------------------------------------------------- dispatch
def lift(fn, w, pc):
    if w & 0x5f000000 == 0x1e000000 and not bits(w, 29, 29):          # scalar FP (M = 0, S = 0)
        return fp_scalar(fn, w, pc)
    if w & 0xff000000 == 0x1f000000:
        return fp_3src(fn, w)
    if w & 0xff3e0c00 == 0x4e280800:
        return crypto_aes(fn, w)
    if w & 0xff208c00 == 0x5e000000:
        return crypto_sha3(fn, w)
    if w & 0xff3e0c00 == 0x5e280800:
        return crypto_sha2(fn, w)
    if w & 0x9f200400 == 0x0e200400:
        return three_same(fn, w)
    if w & 0x9f200c00 == 0x0e200000:
        return three_diff(fn, w)
    if w & 0x9f3e0c00 == 0x0e200800:
        return two_misc(fn, w, scalar=False)
    if w & 0x9f3e0c00 == 0x0e300800:
        return across(fn, w)
    if w & 0x9fe08400 == 0x0e000400:
        return copy(fn, w)
    if w & 0x9ff80400 == 0x0f000400:
        return mod_imm(fn, w)
    if w & 0x9f800400 == 0x0f000400:
        return shift_imm(fn, w, scalar=False)
    if w & 0x9f000400 == 0x0f000000:
        return by_element(fn, w, scalar=False)
    if w & 0xbfe08400 == 0x2e000000:
        return ext(fn, w)
    if w & 0xbf208c00 == 0x0e000800:
        return permute(fn, w)
    if w & 0xbfe08c00 == 0x0e000000 or w & 0xbfe08c00 == 0x0e001000:
        return tbl(fn, w)
    if w & 0xdf200400 == 0x5e200400:
        return scalar_three_same(fn, w)
    if w & 0xdf3e0c00 == 0x5e200800:
        return two_misc(fn, w, scalar=True)
    if w & 0xdf3e0c00 == 0x5e300800:
        return scalar_pairwise(fn, w)
    if w & 0xdfe08400 == 0x5e000400:
        return scalar_copy(fn, w)
    if w & 0xdf800400 == 0x5f000400:
        return shift_imm(fn, w, scalar=True)
    if w & 0xdf000400 == 0x5f000000:
        return by_element(fn, w, scalar=True)
    raise Unimpl('simd')


# ----------------------------------------------------------------------------- scalar FP
RINT = {'n': '__builtin_roundeven', 'a': '__builtin_round', 'p': '__builtin_ceil', 'm': '__builtin_floor',
        'z': '__builtin_trunc', 'x': '__builtin_rint', 'i': '__builtin_rint'}


def rint(size, mode, e):
    f = RINT[mode]
    return '%s%s(%s)' % (f, 'f' if size == 2 else '', e)


def fp_scalar(fn, w, pc):
    ftype, rn, rd = bits(w, 23, 22), bits(w, 9, 5), bits(w, 4, 0)
    if w & 0x7f20fc00 == 0x1e200000:                                  # FP <-> integer
        return fp_int(fn, w)
    if w & 0x7f200000 == 0x1e000000:                                  # FP <-> fixed point
        return fp_fixed(fn, w)
    if w & 0xff207c00 == 0x1e204000:                                  # 1 source
        opcode = bits(w, 20, 15)
        if opcode & 0x3c == 0x04:                                     # FCVT
            dst = opcode & 3
            if ftype == dst or dst == 2 or ftype == 2:
                raise Unimpl('fcvt')
            src = {0: 'f32', 1: 'f64', 3: 'f16'}[ftype]
            dstn = {0: 'f32', 1: 'f64', 3: 'f16'}[dst]
            if src == 'f16':
                v = 'f16_to_f32(%s)' % iread(fn, rn, 1)
                return fwrite(fn, rd, 2, v) if dstn == 'f32' else fwrite(fn, rd, 3, '(f64)' + v)
            if dstn == 'f16':
                return iwrite(fn, rd, 1, 'f%s_to_f16(%s)' % (src[1:], fread(fn, rn, fsz(ftype))))
            return fwrite(fn, rd, 3 if dstn == 'f64' else 2, '(%s)%s' % (dstn, fread(fn, rn, fsz(ftype))))
        size = fsz(ftype)
        a = fread(fn, rn, size)
        ib = 'u32' if size == 2 else 'u64'
        sign = '0x80000000u' if size == 2 else '0x8000000000000000ull'
        if opcode == 0:
            return iwrite(fn, rd, size, iread(fn, rn, size))
        if opcode == 1:
            return iwrite(fn, rd, size, '%s & ~%s' % (iread(fn, rn, size), sign))
        if opcode == 2:
            return iwrite(fn, rd, size, '%s ^ %s' % (iread(fn, rn, size), sign))
        if opcode == 3:
            return fwrite(fn, rd, size, '__builtin_sqrt%s(%s)' % ('f' if size == 2 else '', a))
        modes = {8: 'n', 9: 'p', 10: 'm', 11: 'z', 12: 'a', 14: 'x', 15: 'i'}
        if opcode in modes:
            return fwrite(fn, rd, size, rint(size, modes[opcode], a))
        raise Unimpl('fp1')
    if w & 0xff20fc07 == 0x1e202000:                                  # FCMP, FCMPE
        size = fsz(ftype)
        a = fread(fn, rn, size)
        b = '0.0' if bits(w, 3, 3) else fread(fn, bits(w, 20, 16), size)
        return fcmp_stmt(fn, a, b)
    if w & 0xff201fe0 == 0x1e201000:                                  # FMOV (immediate)
        size = fsz(ftype)
        imm = vfp_expand_imm(bits(w, 20, 13), size)
        return iwrite(fn, rd, size, hx(imm, size == 3))
    if w & 0xff200c00 == 0x1e200400:                                  # FCCMP, FCCMPE
        size = fsz(ftype)
        c0 = cond(fn, bits(w, 15, 12))
        nzcv = bits(w, 3, 0)
        body = fcmp_stmt(fn, fread(fn, rn, size), fread(fn, bits(w, 20, 16), size))
        fn.fk = None
        return 'if %s %s else { NF = %d; ZF = %d; CF = %d; VF = %d; }' % (
            paren(c0), body, nzcv >> 3, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1)
    if w & 0xff200c00 == 0x1e200800:                                  # 2 source
        size = fsz(ftype)
        a, b = fread(fn, rn, size), fread(fn, bits(w, 20, 16), size)
        opcode = bits(w, 15, 12)
        t = 'f32' if size == 2 else 'f64'
        e = {0: '%s * %s', 1: '%s / %s', 2: '%s + %s', 3: '%s - %s', 8: '-(%s * %s)'}
        if opcode in e:
            return fwrite(fn, rd, size, e[opcode] % (a, b))
        fns = {4: 'fmax', 5: 'fmin', 6: 'fmaxnm', 7: 'fminnm'}
        if opcode in fns:
            return fwrite(fn, rd, size, '%s_%s(%s, %s)' % (fns[opcode], t, a, b))
        raise Unimpl('fp2')
    if w & 0xff200c00 == 0x1e200c00:                                  # FCSEL
        size = fsz(ftype)
        c0 = cond(fn, bits(w, 15, 12))
        return fn.setv(rd, '%s ? vz%d(%s) : vz%d(%s)' % (paren(c0), 8 << size, iread(fn, rn, size), 8 << size,
                                                       iread(fn, bits(w, 20, 16), size)))
    raise Unimpl('fp')


def fcmp_stmt(fn, a, b):
    fn.use_flags()
    fn.temps.add('FFA')
    fn.temps.add('FFB')
    fn.fk = ('fcmp', None)
    return '{ f64 a = %s, b = %s; FFA = a; FFB = b; NF = a < b; ZF = a == b; CF = !(a < b); VF = a != a || b != b; }' % (a, b)


def fp_3src(fn, w):
    ftype = bits(w, 23, 22)
    size = fsz(ftype)
    o1, o0 = bits(w, 21, 21), bits(w, 15, 15)
    a, b = fread(fn, bits(w, 9, 5), size), fread(fn, bits(w, 20, 16), size)
    acc = fread(fn, bits(w, 14, 10), size)
    f = '__builtin_fma%s' % ('f' if size == 2 else '')
    e = {(0, 0): '%s(%s, %s, %s)' % (f, a, b, acc), (0, 1): '%s(-%s, %s, %s)' % (f, a, b, acc),
         (1, 0): '%s(-%s, %s, -%s)' % (f, a, b, acc), (1, 1): '%s(%s, %s, -%s)' % (f, a, b, acc)}[(o1, o0)]
    return fwrite(fn, bits(w, 4, 0), size, e)


def fp_int(fn, w):
    sf, ftype, rmode, opcode = bits(w, 31, 31), bits(w, 23, 22), bits(w, 20, 19), bits(w, 18, 16)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    if opcode in (6, 7):                                              # FMOV
        if rmode == 1:
            if not (sf and ftype == 2):
                raise Unimpl('fmov-top')
            if opcode == 6:
                return fn.setx(rd, '%s[1]' % fn.v(rn))
            fn.wv.add(rd)
            return '%s[1] = %s;' % (fn.v(rd), fn.x(rn))
        if rmode != 0:
            raise Unimpl('fmov')
        if ftype == 3:
            raise Unimpl('fmov-half')
        size = fsz(ftype)
        if (size == 3) != bool(sf):
            raise Unimpl('fmov-size')
        if opcode == 6:
            return fn.setr(rd, sf, iread(fn, rn, size))
        return iwrite(fn, rd, size, fn.r(rn, sf))
    size = fsz(ftype)
    t = 'f32' if size == 2 else 'f64'
    if opcode in (2, 3):                                              # SCVTF, UCVTF
        if rmode:
            raise Unimpl('cvtf')
        src = fn.r(rn, sf)
        if opcode == 2:
            src = '(%s)%s' % ('s64' if sf else 's32', src)
        return fwrite(fn, rd, size, '(%s)%s' % (t, src))
    mode = {(0, 0): 'n', (0, 4): 'a', (1, 0): 'p', (2, 0): 'm', (3, 0): 'z'}.get((rmode, opcode & 6))
    if mode is None:
        raise Unimpl('fcvt-int')
    u = opcode & 1
    a = fread(fn, rn, size)
    return fn.setr(rd, sf, 'fcvt_%s(%s, %d)' % ('u' if u else 's', rint(size, mode, a) if mode != 'z' else a,
                                              64 if sf else 32))


def fp_fixed(fn, w):
    sf, ftype, rmode, opcode = bits(w, 31, 31), bits(w, 23, 22), bits(w, 20, 19), bits(w, 18, 16)
    scale = bits(w, 15, 10)
    fbits = 64 - scale
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    size = fsz(ftype)
    t = 'f32' if size == 2 else 'f64'
    if not sf and scale < 32:
        raise Unimpl('fixed-scale')
    if rmode == 0 and opcode in (2, 3):                               # SCVTF/UCVTF #fbits
        src = fn.r(rn, sf)
        if opcode == 2:
            src = '(%s)%s' % ('s64' if sf else 's32', src)
        return fwrite(fn, rd, size, '(%s)((f64)%s / %r)' % (t, src, float(1 << fbits)) if not sf else
                      '(%s)(%s) / (%s)%r' % (t, src, t, float(1 << fbits)))
    if rmode == 3 and opcode in (0, 1):                               # FCVTZS/FCVTZU #fbits
        a = '(f64)%s * %r' % (fread(fn, rn, size), float(1 << fbits))
        return fn.setr(rd, sf, 'fcvt_%s(%s, %d)' % ('u' if opcode else 's', a, 64 if sf else 32))
    raise Unimpl('fixed')


# ----------------------------------------------------------------------------- vector: three same
def three_same(fn, w):
    q, u, size, rm, opcode = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 20, 16), bits(w, 15, 11)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    if opcode == 3:                                                   # logical
        a, b = fn.v(rn), fn.v(rm)
        if u == 0:
            e = {0: '%s & %s', 1: '%s & ~%s', 2: '%s | %s', 3: '%s | ~%s'}[size] % (a, b)
        else:
            d = fn.v(rd)
            e = {0: '%s ^ %s' % (a, b), 1: '(%s & %s) | (~%s & %s)' % (d, a, d, b),
                 2: '(%s & ~%s) | (%s & %s)' % (d, b, a, b), 3: '(%s & %s) | (%s & ~%s)' % (d, b, a, b)}[size]
        return put(fn, rd, q, e)
    if opcode >= 0x18:                                                # FP
        return three_same_fp(fn, w, q, u, size, rm, rn, rd, opcode)
    if size == 3 and not q:
        raise Unimpl('3same-1d')
    A, B = view(fn, rn, size), view(fn, rm, size)
    SA, SB = view(fn, rn, size, 's'), view(fn, rm, size, 's')
    X, Y = (A, B) if u else (SA, SB)
    lanes = (16 if q else 8) >> size
    if opcode == 0x10:
        return put(fn, rd, q, '%s %s %s' % (A, '-' if u else '+', B))
    if opcode == 0x11:
        return put(fn, rd, q, ('%s == %s' % (A, B)) if u else '(%s & %s) != 0' % (A, B))
    if opcode == 0x06:
        return put(fn, rd, q, '%s > %s' % (X, Y))
    if opcode == 0x07:
        return put(fn, rd, q, '%s >= %s' % (X, Y))
    if opcode == 0x0c:
        return put(fn, rd, q, vsel('%s > %s' % (X, Y), X, Y))
    if opcode == 0x0d:
        return put(fn, rd, q, vsel('%s < %s' % (X, Y), X, Y))
    if opcode == 0x12:
        d = view(fn, rd, size)
        return put(fn, rd, q, '%s %s %s * %s' % (d, '-' if u else '+', A, B))
    if opcode == 0x13 and not u:
        return put(fn, rd, q, '%s * %s' % (A, B))
    if opcode == 0x0e:                                                # SABD/UABD
        return put(fn, rd, q, vsel('%s > %s' % (X, Y), '%s - %s' % (A, B), '%s - %s' % (B, A)))
    if opcode == 0x17 and not u:                                      # ADDP
        n = lanes
        ev = shuffle(A, B, [2 * i for i in range(n // 2)] + [16 // (1 << size) + 2 * i for i in range(n // 2)]
                     + [0] * ((16 >> size) - n))
        od = shuffle(A, B, [2 * i + 1 for i in range(n // 2)] + [16 // (1 << size) + 2 * i + 1 for i in range(n // 2)]
                     + [0] * ((16 >> size) - n))
        return put(fn, rd, q, '%s + %s' % (ev, od))
    if opcode == 0x08:                                                # SSHL/USHL
        return put(fn, rd, q, 'v%sshl%d(%s, %s)' % ('u' if u else 's', 8 << size, fn.v(rn), fn.v(rm)))
    if opcode in (0x01, 0x05):                                        # SQADD/UQADD, SQSUB/UQSUB
        return put(fn, rd, q, 'v%sq%s%d(%s, %s)' % ('u' if u else 's', 'add' if opcode == 1 else 'sub', 8 << size,
                                                     fn.v(rn), fn.v(rm)))
    if opcode in (0x00, 0x02):                                        # SHADD/UHADD, SRHADD/URHADD
        return put(fn, rd, q, 'v%s%shadd%d(%s, %s)' % ('u' if u else 's', 'r' if opcode == 2 else '', 8 << size,
                                                      fn.v(rn), fn.v(rm)))
    if opcode in (0x14, 0x15):                                        # SMAXP/UMAXP, SMINP/UMINP
        n = lanes
        ev = shuffle(X, Y, [2 * i for i in range(n // 2)] + [(16 >> size) + 2 * i for i in range(n // 2)]
                     + [0] * ((16 >> size) - n))
        od = shuffle(X, Y, [2 * i + 1 for i in range(n // 2)] + [(16 >> size) + 2 * i + 1 for i in range(n // 2)]
                     + [0] * ((16 >> size) - n))
        op = '>' if opcode == 0x14 else '<'
        return '{ %s e = %s, o = %s; %s }' % (vt(size, 'u' if u else 's'), ev, od, put(fn, rd, q, vsel('e %s o' % op, 'e', 'o')))
    raise Unimpl('3same-%x%s' % (opcode, 'u' if u else ''))


def three_same_fp(fn, w, q, u, size, rm, rn, rd, opcode):
    sz = 3 if size & 1 else 2
    hi = size >> 1
    if sz == 3 and not q:
        raise Unimpl('3same-fp-1d')
    t = vt(sz, 'f')
    A, B = view(fn, rn, sz, 'f'), view(fn, rm, sz, 'f')
    key = (u, hi, opcode)
    simple = {(0, 0, 0x1a): '%s + %s', (0, 1, 0x1a): '%s - %s', (1, 0, 0x1b): '%s * %s', (1, 0, 0x1f): '%s / %s',
              (0, 0, 0x1c): '%s == %s', (1, 0, 0x1c): '%s >= %s', (1, 1, 0x1c): '%s > %s'}
    if key in simple:
        return put(fn, rd, q, simple[key] % (A, B))
    if key in ((0, 0, 0x19), (0, 1, 0x19)):                         # FMLA, FMLS (fused)
        d = view(fn, rd, sz, 'f')
        neg = '-' if hi else ''
        return put(fn, rd, q, '__builtin_elementwise_fma(%s%s, %s, %s)' % (neg, A, B, d))
    if key == (1, 1, 0x1a):                                          # FABD
        return put(fn, rd, q, 'vfabs%d(%s - %s)' % (8 << sz, A, B))
    if key in ((1, 0, 0x1d), (1, 1, 0x1d)):                         # FACGE, FACGT
        return put(fn, rd, q, 'vfabs%d(%s) %s vfabs%d(%s)' % (8 << sz, A, '>=' if not hi else '>', 8 << sz, B))
    if key == (1, 0, 0x1a):                                          # FADDP
        n = (16 if q else 8) >> sz
        full = 16 >> sz
        ev = shuffle(A, B, [2 * i for i in range(n // 2)] + [full + 2 * i for i in range(n // 2)] + [0] * (full - n))
        od = shuffle(A, B, [2 * i + 1 for i in range(n // 2)] + [full + 2 * i + 1 for i in range(n // 2)] + [0] * (full - n))
        return put(fn, rd, q, '%s + %s' % (ev, od))
    names = {(0, 0, 0x1e): 'fmax', (0, 1, 0x1e): 'fmin', (0, 0, 0x18): 'fmaxnm', (0, 1, 0x18): 'fminnm',
             (0, 0, 0x1f): 'frecps', (0, 1, 0x1f): 'frsqrts', (0, 0, 0x1b): 'fmulx'}
    if key in names:
        return put(fn, rd, q, 'v%s_f%d(%s, %s)' % (names[key], 8 << sz, fn.v(rn), fn.v(rm)))
    if key in ((1, 0, 0x1e), (1, 1, 0x1e), (1, 0, 0x18), (1, 1, 0x18)):   # FMAXP, FMINP, FMAXNMP, FMINNMP
        n = (16 if q else 8) >> sz
        full = 16 >> sz
        ev = shuffle(A, B, [2 * i for i in range(n // 2)] + [full + 2 * i for i in range(n // 2)] + [0] * (full - n))
        od = shuffle(A, B, [2 * i + 1 for i in range(n // 2)] + [full + 2 * i + 1 for i in range(n // 2)] + [0] * (full - n))
        nm = {0x1e: 'fmax', 0x18: 'fmaxnm'}[opcode].replace('max', 'min' if hi else 'max')
        return put(fn, rd, q, 'v%s_f%d((v128)%s, (v128)%s)' % (nm, 8 << sz, ev, od))
    raise Unimpl('3same-fp-%x%s%s' % (opcode, 'u' if u else '', 'b' if hi else ''))


def scalar_three_same(fn, w):
    u, size, rm, opcode = bits(w, 29, 29), bits(w, 23, 22), bits(w, 20, 16), bits(w, 15, 11)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    if opcode >= 0x18:
        sz = 3 if size & 1 else 2
        hi = size >> 1
        A, B = fread(fn, rn, sz), fread(fn, rm, sz)
        key = (u, hi, opcode)
        t = 'f32' if sz == 2 else 'f64'
        mask_ = '(u32)-(s32)' if sz == 2 else '(u64)-(s64)'
        cmps = {(0, 0, 0x1c): '==', (1, 0, 0x1c): '>=', (1, 1, 0x1c): '>'}
        if key in cmps:
            return iwrite(fn, rd, sz, '%s(%s %s %s)' % (mask_, A, cmps[key], B))
        if key in ((1, 0, 0x1d), (1, 1, 0x1d)):
            op = '>=' if not hi else '>'
            return iwrite(fn, rd, sz, '%s(__builtin_fabs%s(%s) %s __builtin_fabs%s(%s))' % (
                mask_, 'f' if sz == 2 else '', A, op, 'f' if sz == 2 else '', B))
        if key == (1, 1, 0x1a):
            return fwrite(fn, rd, sz, '__builtin_fabs%s(%s - %s)' % ('f' if sz == 2 else '', A, B))
        names = {(0, 0, 0x1f): 'frecps', (0, 1, 0x1f): 'frsqrts', (0, 0, 0x1b): 'fmulx'}
        if key in names:
            return fwrite(fn, rd, sz, '%s_%s(%s, %s)' % (names[key], t, A, B))
        raise Unimpl('s3same-fp-%x' % opcode)
    if size != 3:
        raise Unimpl('s3same-size')
    A, B = iread(fn, rn, 3), iread(fn, rm, 3)
    SA, SB = '(s64)' + A, '(s64)' + B
    if opcode == 0x10:
        return iwrite(fn, rd, 3, '%s %s %s' % (A, '-' if u else '+', B))
    if opcode == 0x11:
        return iwrite(fn, rd, 3, '(u64)-(s64)(%s)' % (('%s == %s' % (A, B)) if u else '(%s & %s) != 0' % (A, B)))
    if opcode in (6, 7):
        op = '>' if opcode == 6 else '>='
        return iwrite(fn, rd, 3, '(u64)-(s64)(%s %s %s)' % (A if u else SA, op, B if u else SB))
    if opcode == 8:
        return fn.setv(rd, 'vlo(v%sshl64(%s, %s))' % ('u' if u else 's', fn.v(rn), fn.v(rm)))
    raise Unimpl('s3same-%x' % opcode)


# ----------------------------------------------------------------------------- vector: three different
def three_diff(fn, w):
    q, u, size, rm, opcode = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 20, 16), bits(w, 15, 12)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    if opcode == 0xe and not u:                                       # PMULL, PMULL2
        if size == 0:
            return fn.setv(rd, 'pmull8(%s, %s, %d)' % (fn.v(rn), fn.v(rm), q))
        if size == 3:
            return fn.setv(rd, 'pmull64(%s, %s, %d)' % (fn.v(rn), fn.v(rm), q))
        raise Unimpl('pmull')
    if size == 3:
        raise Unimpl('3diff-size')
    k = 'u' if u else 's'
    half = vt(size, k, 0)
    wide = vt(size + 1, k)
    n = 8 >> size

    def widen(r):
        idx = list(range(n, 2 * n)) if q else list(range(n))
        return '__builtin_convertvector(%s, %s)' % (shuffle(view(fn, r, size, k), view(fn, r, size, k), idx), wide)
    if opcode == 0xc:                                                 # SMULL/UMULL
        return fn.setv(rd, '(v128)(%s * %s)' % (widen(rn), widen(rm)))
    if opcode in (0x8, 0xa):                                          # SMLAL/UMLAL, SMLSL/UMLSL
        return fn.setv(rd, '(v128)(%s %s %s * %s)' % (view(fn, rd, size + 1, k), '+' if opcode == 8 else '-',
                                                     widen(rn), widen(rm)))
    if opcode in (0x0, 0x2):                                          # SADDL/UADDL, SSUBL/USUBL
        return fn.setv(rd, '(v128)(%s %s %s)' % (widen(rn), '+' if opcode == 0 else '-', widen(rm)))
    if opcode in (0x1, 0x3):                                          # SADDW, SSUBW
        return fn.setv(rd, '(v128)(%s %s %s)' % (view(fn, rn, size + 1, k), '+' if opcode == 1 else '-', widen(rm)))
    raise Unimpl('3diff-%x' % opcode)


# ----------------------------------------------------------------------------- vector: two-register misc
def two_misc(fn, w, scalar):
    q, u, size, opcode = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 16, 12)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    if scalar:
        q = 1
    if opcode >= 0x0c and opcode != 0x12 and opcode != 0x13 and opcode != 0x14 and size >> 1 == 0 or \
            opcode >= 0x0c and size >> 1 == 1 and opcode not in (0x12, 0x13, 0x14):
        return two_misc_fp(fn, w, q, u, size, opcode, rn, rd, scalar)
    if scalar:
        if size != 3:
            if opcode in (0x12, 0x14):
                raise Unimpl('s2misc-narrow')
            raise Unimpl('s2misc-size')
        A = iread(fn, rn, 3)
        if opcode == 0x0b:
            return iwrite(fn, rd, 3, ('(u64)0 - %s' % A) if u else '(u64)((s64)%s < 0 ? -(s64)%s : (s64)%s)' % (A, A, A))
        cm = {(0, 0x08): '>', (1, 0x08): '>=', (0, 0x09): '==', (1, 0x09): '<=', (0, 0x0a): '<'}
        if (u, opcode) in cm:
            return iwrite(fn, rd, 3, '(u64)-(s64)((s64)%s %s 0)' % (A, cm[(u, opcode)]))
        raise Unimpl('s2misc-%x' % opcode)
    A = view(fn, rn, size)
    SA = view(fn, rn, size, 's')
    lanes = 16 >> size
    if opcode == 0x00 and not u or opcode == 0x00 and u or opcode == 0x01 and not u:   # REV64, REV32, REV16
        grp = {(0, 0): 64, (1, 0): 32, (0, 1): 16}[(u, opcode)]
        per = grp // (8 << size)
        if per < 2:
            raise Unimpl('rev')
        idx = [g * per + (per - 1 - i) for g in range(lanes // per) for i in range(per)]
        return put(fn, rd, q, shuffle(A, A, idx))
    if opcode == 0x05:
        if u == 0 and size == 0:
            return put(fn, rd, q, 'vcnt8(%s)' % fn.v(rn))
        if u == 1 and size == 0:
            return put(fn, rd, q, '~%s' % fn.v(rn))
        if u == 1 and size == 1:
            return put(fn, rd, q, 'vrbit8(%s)' % fn.v(rn))
    if opcode == 0x04:                                                # CLS/CLZ
        return put(fn, rd, q, 'v%s%d(%s)' % ('clz' if u else 'cls', 8 << size, fn.v(rn)))
    if opcode == 0x0b:                                                # ABS / NEG
        if u:
            return put(fn, rd, q, '-%s' % A)
        return put(fn, rd, q, vsel('%s < 0' % SA, '-%s' % SA, SA))
    cm = {(0, 0x08): '>', (1, 0x08): '>=', (0, 0x09): '==', (1, 0x09): '<=', (0, 0x0a): '<'}
    if (u, opcode) in cm:
        return put(fn, rd, q, '%s %s 0' % (SA, cm[(u, opcode)]))
    if opcode == 0x12 and not u or opcode == 0x14:                    # XTN, SQXTN/UQXTN
        if size == 3:
            raise Unimpl('xtn')
        wide = size + 1
        n = 8 >> size
        if opcode == 0x12:
            src = view(fn, rn, wide)
            narrow = '__builtin_convertvector(%s, %s)' % (src, vt(size, 'u', 0))
        else:
            narrow = '(%s)v%sqxtn%d(%s)' % (vt(size, 'u', 0), 'u' if u else 's', 8 << wide, fn.v(rn))
        return narrow_put(fn, rd, q, size, narrow)
    if opcode == 0x13 and u:                                          # SHLL
        n = 8 >> size
        idx = list(range(n, 2 * n)) if q else list(range(n))
        wide = '__builtin_convertvector(%s, %s)' % (shuffle(A, A, idx), vt(size + 1))
        return fn.setv(rd, '(v128)(%s << %d)' % (wide, 8 << size))
    if opcode == 0x02 or opcode == 0x06:                              # SADDLP/UADDLP, SADALP/UADALP
        k = 'u' if u else 's'
        n = lanes if q else lanes // 2
        ev = shuffle(view(fn, rn, size, k), view(fn, rn, size, k), [2 * i for i in range(n // 2)])
        od = shuffle(view(fn, rn, size, k), view(fn, rn, size, k), [2 * i + 1 for i in range(n // 2)])
        wt = vt(size + 1, k, q)
        e = '__builtin_convertvector(%s, %s) + __builtin_convertvector(%s, %s)' % (ev, wt, od, wt)
        if opcode == 0x06:
            e = '%s + (%s)%s' % (e, wt, ('%s' if q else '(%s)%s[0]') % ((fn.v(rd),) if q else ('v1u64', fn.v(rd))))
            raise Unimpl('sadalp')
        if q:
            return fn.setv(rd, '(v128)(%s)' % e)
        return fn.setv(rd, 'vz64(((v1u64)(%s))[0])' % e)
    raise Unimpl('2misc-%x%s' % (opcode, 'u' if u else ''))


def narrow_put(fn, rd, q, size, narrow):
    """Write an 8-byte narrowed vector to the low half (Q = 0, upper zeroed) or the high half (Q = 1, low kept)."""
    if q:
        return fn.setv(rd, '(v128){%s[0], ((v1u64)(%s))[0]}' % (fn.v(rd), narrow))
    return fn.setv(rd, 'vz64(((v1u64)(%s))[0])' % narrow)


def two_misc_fp(fn, w, q, u, size, opcode, rn, rd, scalar):
    sz = 3 if size & 1 else 2
    hi = size >> 1
    if not scalar and sz == 3 and not q and opcode not in (0x16, 0x17):
        raise Unimpl('2misc-fp-1d')
    t = vt(sz, 'f')
    A = view(fn, rn, sz, 'f')
    key = (u, hi, opcode)

    def out(e_vec, e_scalar_kind=None):
        if scalar:
            return fn.setv(rd, 'vlo%d((v128)(%s))' % (8 << sz, e_vec))
        return put(fn, rd, q, e_vec)
    if key in ((0, 0, 0x16), (0, 0, 0x17)) and not scalar:           # FCVTN / FCVTL (size<0> = 1: double)
        if opcode == 0x16:
            if sz != 3:
                raise Unimpl('fcvtn-half')
            narrow = '__builtin_convertvector(%s, v2f32)' % view(fn, rn, 3, 'f')
            return narrow_put(fn, rd, q, 2, narrow)
        if sz != 3:
            raise Unimpl('fcvtl-half')
        src = view(fn, rn, 2, 'f')
        return fn.setv(rd, '(v128)__builtin_convertvector(%s, v2f64)' % shuffle(src, src, [2, 3] if q else [0, 1]))
    if key in ((0, 0, 0x1d), (1, 0, 0x1d)):                         # SCVTF, UCVTF (vector or scalar)
        src = view(fn, rn, sz, 'u' if u else 's')
        return out('__builtin_convertvector(%s, %s)' % (src, t))
    modes = {(0, 0, 0x1a): ('n', 0), (1, 0, 0x1a): ('n', 1), (0, 0, 0x1b): ('m', 0), (1, 0, 0x1b): ('m', 1),
             (0, 0, 0x1c): ('a', 0), (1, 0, 0x1c): ('a', 1), (0, 1, 0x1a): ('p', 0), (1, 1, 0x1a): ('p', 1),
             (0, 1, 0x1b): ('z', 0), (1, 1, 0x1b): ('z', 1)}
    if key in modes:                                                  # FCVT{N,M,A,P,Z}{S,U}
        m, uu = modes[key]
        return out('vfcvt%s%d_%s(%s)' % ('u' if uu else 's', 8 << sz, m, fn.v(rn)))
    rints = {(0, 0, 0x18): 'n', (1, 0, 0x18): 'a', (0, 1, 0x18): 'p', (0, 0, 0x19): 'm', (1, 0, 0x19): 'x',
             (0, 1, 0x19): 'z', (1, 1, 0x19): 'i'}
    if key in rints and not scalar:
        return put(fn, rd, q, 'vfrint%d_%s(%s)' % (8 << sz, rints[key], fn.v(rn)))
    if key == (0, 1, 0x1d):
        return out('vfrecpe%d(%s)' % (8 << sz, fn.v(rn)))
    if key == (1, 1, 0x1d):
        return out('vfrsqrte%d(%s)' % (8 << sz, fn.v(rn)))
    if key in ((0, 1, 0x0f), (1, 1, 0x0f)) and not scalar:           # FABS, FNEG
        bitm = '0x80000000u' if sz == 2 else '0x8000000000000000ull'
        iv = view(fn, rn, sz)
        return put(fn, rd, q, ('%s & ~%s' if not u else '%s ^ %s') % (iv, splat(vt(sz), bitm, 16 >> sz)))
    if key == (1, 1, 0x1f) and not scalar:                           # FSQRT
        return put(fn, rd, q, 'vfsqrt%d(%s)' % (8 << sz, fn.v(rn)))
    cmz = {(0, 1, 0x0c): '>', (1, 1, 0x0c): '>=', (0, 1, 0x0d): '==', (1, 1, 0x0d): '<=', (0, 1, 0x0e): '<'}
    if key in cmz:
        return out('%s %s 0' % (A, cmz[key]))
    raise Unimpl('2misc-fp-%x%s%s%s' % (opcode, 'u' if u else '', 'b' if hi else '', 's' if scalar else ''))


# ----------------------------------------------------------------------------- across lanes, pairwise
def across(fn, w):
    q, u, size, opcode = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 16, 12)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    lanes = (16 if q else 8) >> size
    if opcode == 0x1b and not u:
        return iwrite(fn, rd, size, '(u%d)vaddv%d(%s, %d)' % (8 << size, 8 << size, fn.v(rn), lanes))
    if opcode == 0x03:
        return iwrite(fn, rd, size + 1, 'v%saddlv%d(%s, %d)' % ('u' if u else 's', 8 << size, fn.v(rn), lanes))
    if opcode in (0x0a, 0x1a):
        return iwrite(fn, rd, size, 'v%s%sv%d(%s, %d)' % ('u' if u else 's', 'max' if opcode == 0x0a else 'min',
                                                        8 << size, fn.v(rn), lanes))
    if u and opcode in (0x0c, 0x0f) and q and size in (0, 2):     # FMAXNMV/FMAXV (size<1>=0), FMINNMV/FMINV
        nm = ('fmaxnm' if opcode == 0x0c else 'fmax') if size == 0 else ('fminnm' if opcode == 0x0c else 'fmin')
        return fwrite(fn, rd, 2, 'v%sv_f32(%s)' % (nm, fn.v(rn)))
    raise Unimpl('across-%x' % opcode)


def scalar_pairwise(fn, w):
    u, size, opcode = bits(w, 29, 29), bits(w, 23, 22), bits(w, 16, 12)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    if opcode == 0x1b and not u and size == 3:                        # ADDP d
        return iwrite(fn, rd, 3, '%s[0] + %s[1]' % (fn.v(rn), fn.v(rn)))
    if u:
        sz = 3 if size & 1 else 2
        A = view(fn, rn, sz, 'f')
        if opcode == 0x0d and not size >> 1:                         # FADDP
            return fwrite(fn, rd, sz, '%s[0] + %s[1]' % (A, A))
        names = {(0x0f, 0): 'fmax', (0x0f, 1): 'fmin', (0x0c, 0): 'fmaxnm', (0x0c, 1): 'fminnm'}
        if (opcode, size >> 1) in names:
            return fwrite(fn, rd, sz, '%s_f%d(%s[0], %s[1])' % (names[(opcode, size >> 1)], 8 << sz, A, A))
    raise Unimpl('spair-%x' % opcode)


# ----------------------------------------------------------------------------- copy, immediates, shifts
def copy(fn, w):
    q, op, imm5, imm4 = bits(w, 30, 30), bits(w, 29, 29), bits(w, 20, 16), bits(w, 14, 11)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    if imm5 & 0xf == 0:
        raise Unimpl('copy-imm5')
    size = (imm5 & -imm5).bit_length() - 1
    idx = imm5 >> (size + 1)
    lanes = 16 >> size
    if op == 1:                                                       # INS (element)
        src = imm4 >> size
        t = vt(size)
        return '{ %s t = (%s)%s; t[%d] = ((%s)%s)[%d]; %s }' % (t, t, fn.v(rd), idx, t, fn.v(rn), src,
                                                                fn.setv(rd, '(v128)t'))
    if imm4 == 0:                                                     # DUP (element)
        if size == 3 and not q:
            raise Unimpl('dup-1d')
        A = view(fn, rn, size)
        return put(fn, rd, q, shuffle(A, A, [idx] * lanes))
    if imm4 == 1:                                                     # DUP (general)
        if size == 3 and not q:
            raise Unimpl('dup-1d')
        return put(fn, rd, q, splat(vt(size), '(u%d)%s' % (8 << size, fn.x(rn)), lanes))
    if imm4 == 3:                                                     # INS (general)
        t = vt(size)
        return '{ %s t = (%s)%s; t[%d] = (u%d)%s; %s }' % (t, t, fn.v(rd), idx, 8 << size, fn.x(rn),
                                                         fn.setv(rd, '(v128)t'))
    if imm4 == 7:                                                     # UMOV
        return fn.setr(rd, q, '((%s)%s)[%d]' % (vt(size), fn.v(rn), idx))
    if imm4 == 5:                                                     # SMOV
        return fn.setr(rd, q, '(%s)((%s)%s)[%d]' % ('u64' if q else 'u32', vt(size, 's'), fn.v(rn), idx) if q else
                       '(u32)(s32)((%s)%s)[%d]' % (vt(size, 's'), fn.v(rn), idx))
    raise Unimpl('copy-%x' % imm4)


def scalar_copy(fn, w):
    imm5, rn, rd = bits(w, 20, 16), bits(w, 9, 5), bits(w, 4, 0)
    if bits(w, 29, 29) or bits(w, 14, 11) or imm5 & 0xf == 0:
        raise Unimpl('scopy')
    size = (imm5 & -imm5).bit_length() - 1
    idx = imm5 >> (size + 1)
    return iwrite(fn, rd, size, '((%s)%s)[%d]' % (vt(size), fn.v(rn), idx))


def adv_simd_expand_imm(op, cmode, imm8):
    c = cmode >> 1
    if c == 0:
        return rep(imm8, 32)
    if c == 1:
        return rep(imm8 << 8, 32)
    if c == 2:
        return rep(imm8 << 16, 32)
    if c == 3:
        return rep(imm8 << 24, 32)
    if c == 4:
        return rep(imm8, 16)
    if c == 5:
        return rep(imm8 << 8, 16)
    if c == 6:
        return rep((imm8 << 8) | 0xff, 32) if not cmode & 1 else rep((imm8 << 16) | 0xffff, 32)
    if not cmode & 1:
        if not op:
            return rep(imm8, 8)
        v = 0
        for i in range(8):
            if imm8 >> i & 1:
                v |= 0xff << (8 * i)
        return v
    if not op:
        return rep(vfp_expand_imm(imm8, 2), 32)
    return vfp_expand_imm(imm8, 3)


def rep(v, n):
    out = 0
    for i in range(0, 64, n):
        out |= (v & mask(n)) << i
    return out


def mod_imm(fn, w):
    q, op, cmode, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 15, 12), bits(w, 4, 0)
    if bits(w, 11, 11):
        raise Unimpl('modimm-o2')
    imm8 = (bits(w, 18, 16) << 5) | bits(w, 9, 5)
    imm = adv_simd_expand_imm(op, cmode, imm8)
    if cmode == 0xf and op and not q:
        raise Unimpl('fmov-2d')
    orr_bic = (cmode < 8 and cmode & 1) or (cmode in (9, 11))
    if orr_bic:
        k = '(v128){0x%xull, 0x%xull}' % (imm, imm if q else 0)
        if op:                                                        # BIC
            return put(fn, rd, q, '%s & ~%s' % (fn.v(rd), '(v128){0x%xull, 0x%xull}' % (imm, imm)))
        return put(fn, rd, q, '%s | %s' % (fn.v(rd), k))
    if op and cmode != 0xe and cmode != 0xf:                         # MVNI
        imm = ~imm & mask(64)
    return fn.setv(rd, '(v128){0x%xull, 0x%xull}' % (imm, imm if q else 0))


def shift_imm(fn, w, scalar):
    q, u, immh, immb, opcode = bits(w, 30, 30), bits(w, 29, 29), bits(w, 22, 19), bits(w, 18, 16), bits(w, 15, 11)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    if immh == 0:
        raise Unimpl('shimm-immh')
    size = immh.bit_length() - 1
    esize = 8 << size
    imm = (immh << 3) | immb
    rshift = 2 * esize - imm
    lshift = imm - esize
    if scalar:
        if opcode in (0x1c, 0x1f):
            if size not in (2, 3):
                raise Unimpl('s-fixed-size')
            fbits = rshift
            t = 'f32' if size == 2 else 'f64'
            if opcode == 0x1c:                                        # SCVTF/UCVTF fixed (scalar)
                src = '(%s)%s' % (('u' if u else 's') + str(esize), iread(fn, rn, size))
                return fwrite(fn, rd, size, '(%s)((f64)%s / %r)' % (t, src, float(1 << fbits)) if size == 2 else
                              '(%s)%s / %r' % (t, src, float(1 << fbits)))
            a = '(f64)%s * %r' % (fread(fn, rn, size), float(1 << fbits))
            return iwrite(fn, rd, size, 'fcvt_%s(%s, %d)' % ('u' if u else 's', a, esize))
        if size != 3:
            raise Unimpl('s-shimm-size')
        A = iread(fn, rn, 3)
        if opcode == 0x00:
            e = '(u64)((s64)%s >> %d)' % (A, min(rshift, 63)) if not u else ('%s >> %d' % (A, rshift) if rshift < 64 else '0')
            return iwrite(fn, rd, 3, e)
        if opcode == 0x0a and not u:
            return iwrite(fn, rd, 3, '%s << %d' % (A, lshift))
        if opcode == 0x02:                                            # SSRA/USRA
            e = '(u64)((s64)%s >> %d)' % (A, min(rshift, 63)) if not u else ('%s >> %d' % (A, rshift) if rshift < 64 else '0')
            return iwrite(fn, rd, 3, '%s + %s' % (iread(fn, rd, 3), e))
        raise Unimpl('s-shimm-%x' % opcode)
    if size == 3 and not q:
        raise Unimpl('shimm-1d')
    lanes = 16 >> size
    A, SA = view(fn, rn, size), view(fn, rn, size, 's')
    if opcode == 0x00:                                                # SSHR/USHR
        if u:
            return put(fn, rd, q, ('%s >> %d' % (A, rshift)) if rshift < esize else '(v128){0, 0}')
        return put(fn, rd, q, '%s >> %d' % (SA, min(rshift, esize - 1)))
    if opcode == 0x02:                                                # SSRA/USRA
        d = view(fn, rd, size)
        sh = ('%s >> %d' % (A, rshift)) if u and rshift < esize else ('(%s)(%s >> %d)' % (vt(size), SA, min(rshift, esize - 1))
                                                                       if not u else '0')
        return put(fn, rd, q, '%s + %s' % (d, sh))
    if opcode == 0x0a and not u:                                      # SHL
        return put(fn, rd, q, '%s << %d' % (A, lshift))
    if opcode == 0x0a and u:                                          # SLI
        d = view(fn, rd, size)
        m = mask(esize) >> (esize - lshift) if lshift else 0
        return put(fn, rd, q, '(%s << %d) | (%s & %s)' % (A, lshift, d, splat(vt(size), hx(m, size == 3), lanes)))
    if opcode == 0x08 and u:                                          # SRI
        d = view(fn, rd, size)
        m = (mask(esize) << (esize - rshift)) & mask(esize) if rshift < esize else mask(esize)
        sh = ('%s >> %d' % (A, rshift)) if rshift < esize else splat(vt(size), '0', lanes)
        return put(fn, rd, q, '(%s) | (%s & %s)' % (sh, d, splat(vt(size), hx(m, size == 3), lanes)))
    if opcode == 0x14:                                                # SSHLL/USHLL (and SXTL/UXTL)
        if size == 3:
            raise Unimpl('sshll')
        k = 'u' if u else 's'
        n = 8 >> size
        idx = list(range(n, 2 * n)) if q else list(range(n))
        src = view(fn, rn, size, k)
        wide = '__builtin_convertvector(%s, %s)' % (shuffle(src, src, idx), vt(size + 1, k))
        return fn.setv(rd, '(v128)(%s << %d)' % (wide, lshift) if lshift else '(v128)%s' % wide)
    if opcode == 0x10 and not u:                                      # SHRN
        if size == 3:
            raise Unimpl('shrn')
        src = view(fn, rn, size + 1)
        nar = '__builtin_convertvector(%s >> %d, %s)' % (src, rshift, vt(size, 'u', 0))
        return narrow_put(fn, rd, q, size, nar)
    if opcode in (0x1c, 0x1f):                                        # SCVTF/UCVTF/FCVTZS/FCVTZU (fixed, vector)
        if size not in (2, 3):
            raise Unimpl('fixed-v-size')
        t = vt(size, 'f')
        scale = splat(t, repr(float(1 << rshift)) + ('f' if size == 2 else ''), lanes)
        if opcode == 0x1c:
            return put(fn, rd, q, '__builtin_convertvector(%s, %s) / %s' % (view(fn, rn, size, 'u' if u else 's'), t, scale))
        return put(fn, rd, q, 'vfcvt%s%d_z((v128)(%s * %s))' % ('u' if u else 's', esize, view(fn, rn, size, 'f'), scale))
    if opcode in (0x12, 0x13, 0x11, 0x10) and (u or opcode != 0x10):   # SQSHRN, SQRSHRN, UQRSHRN, SQRSHRUN, ...
        if size == 3:
            raise Unimpl('qshrn')
        name = {(0, 0x12): 'sqshrn', (1, 0x12): 'uqshrn', (0, 0x13): 'sqrshrn', (1, 0x13): 'uqrshrn',
                (0, 0x11): 'rshrn', (1, 0x10): 'sqshrun', (1, 0x11): 'sqrshrun'}[(u, opcode)]
        nar = '(%s)v%s%d(%s, %d)' % (vt(size, 'u', 0), name, 16 << size, fn.v(rn), 2 * esize - imm)
        return narrow_put(fn, rd, q, size, nar)
    raise Unimpl('shimm-%x%s' % (opcode, 'u' if u else ''))


def by_element(fn, w, scalar):
    q, u, size, l, m, rm4, opcode, h = (bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 21, 21),
                                        bits(w, 20, 20), bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 11))
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    if scalar:
        q = 1
    fpop = opcode in (0x1, 0x5, 0x9) and (u == 0 or opcode == 0x9)
    if fpop:
        if size == 2:
            sz, idx, rm = 2, (h << 1) | l, (m << 4) | rm4
        elif size == 3 and not l:
            sz, idx, rm = 3, h, (m << 4) | rm4
        else:
            raise Unimpl('elem-fp-size')
        t = vt(sz, 'f')
        lanes = 16 >> sz
        el = '((%s)%s)[%d]' % (t, fn.v(rm), idx)
        if scalar:
            a = fread(fn, rn, sz)
            if opcode == 0x9 and not u:
                return fwrite(fn, rd, sz, '%s * %s' % (a, el))
            if opcode == 0x9:
                return fwrite(fn, rd, sz, 'fmulx_%s(%s, %s)' % ('f32' if sz == 2 else 'f64', a, el))
            f = '__builtin_fma%s' % ('f' if sz == 2 else '')
            return fwrite(fn, rd, sz, '%s(%s%s, %s, %s)' % (f, '-' if opcode == 5 else '', a, el, fread(fn, rd, sz)))
        if sz == 3 and not q:
            raise Unimpl('elem-1d')
        A = view(fn, rn, sz, 'f')
        sp = shuffle(view(fn, rm, sz, 'f'), view(fn, rm, sz, 'f'), [idx] * lanes)
        if opcode == 0x9 and not u:
            return put(fn, rd, q, '%s * %s' % (A, sp))
        if opcode == 0x9:
            return put(fn, rd, q, 'vfmulx_f%d(%s, (v128)%s)' % (8 << sz, fn.v(rn), sp))
        return put(fn, rd, q, '__builtin_elementwise_fma(%s%s, %s, %s)' % ('-' if opcode == 5 else '', A, sp,
                                                                           view(fn, rd, sz, 'f')))
    if scalar:
        raise Unimpl('selem-int')
    if size == 1:
        idx, rm = (h << 2) | (l << 1) | m, rm4
    elif size == 2:
        idx, rm = (h << 1) | l, (m << 4) | rm4
    else:
        raise Unimpl('elem-int-size')
    lanes = 16 >> size
    A = view(fn, rn, size)
    sp = shuffle(view(fn, rm, size), view(fn, rm, size), [idx] * lanes)
    if opcode == 0x8 and not u:
        return put(fn, rd, q, '%s * %s' % (A, sp))
    if opcode in (0x0, 0x4) and u:
        return put(fn, rd, q, '%s %s %s * %s' % (view(fn, rd, size), '+' if opcode == 0 else '-', A, sp))
    if opcode in (0xa, 0x2, 0x6):                                     # SMULL/UMULL, SMLAL, SMLSL (by element)
        k = 'u' if u else 's'
        n = 8 >> size
        src = view(fn, rn, size, k)
        idx2 = list(range(n, 2 * n)) if q else list(range(n))
        wa = '__builtin_convertvector(%s, %s)' % (shuffle(src, src, idx2), vt(size + 1, k))
        es = view(fn, rm, size, k)
        wb = '__builtin_convertvector(%s, %s)' % (shuffle(es, es, [idx] * n), vt(size + 1, k))
        if opcode == 0xa:
            return fn.setv(rd, '(v128)(%s * %s)' % (wa, wb))
        return fn.setv(rd, '(v128)(%s %s %s * %s)' % (view(fn, rd, size + 1, k), '+' if opcode == 2 else '-', wa, wb))
    raise Unimpl('elem-%x%s' % (opcode, 'u' if u else ''))


# ----------------------------------------------------------------------------- permutes
def ext(fn, w):
    q, rm, imm4, rn, rd = bits(w, 30, 30), bits(w, 20, 16), bits(w, 14, 11), bits(w, 9, 5), bits(w, 4, 0)
    if not q and imm4 >= 8:
        raise Unimpl('ext')
    n = 16 if q else 8
    idx = []
    for i in range(16):
        j = imm4 + i
        if i >= n:
            idx.append(0)
        elif j < n:
            idx.append(j)
        else:
            idx.append(16 + j - n)
    return put(fn, rd, q, shuffle(view(fn, rn, 0), view(fn, rm, 0), idx))


def permute(fn, w):
    q, size, rm, opcode, rn, rd = (bits(w, 30, 30), bits(w, 23, 22), bits(w, 20, 16), bits(w, 14, 12),
                                   bits(w, 9, 5), bits(w, 4, 0))
    if size == 3 and not q:
        raise Unimpl('perm-1d')
    full = 16 >> size
    n = full if q else full // 2
    idx = []
    for i in range(n):
        if opcode in (1, 5):                                          # UZP1/UZP2
            j = 2 * i + (opcode == 5)
            idx.append(j if j < n else full + j - n)
        elif opcode in (2, 6):                                        # TRN1/TRN2
            base = (i & ~1) + (opcode == 6)
            idx.append(base if i % 2 == 0 else full + base)
        elif opcode in (3, 7):                                        # ZIP1/ZIP2
            base = (n // 2 if opcode == 7 else 0) + i // 2
            idx.append(base if i % 2 == 0 else full + base)
        else:
            raise Unimpl('perm')
    idx += [0] * (full - n)
    return put(fn, rd, q, shuffle(view(fn, rn, size), view(fn, rm, size), idx))


def tbl(fn, w):
    q, rm, ln, op, rn, rd = bits(w, 30, 30), bits(w, 20, 16), bits(w, 14, 13), bits(w, 12, 12), bits(w, 9, 5), bits(w, 4, 0)
    regs = [fn.v((rn + i) % 32) for i in range(ln + 1)]
    dflt = fn.v(rd) if op else '(v128){0, 0}'
    return '{ v128 t[%d] = {%s}; %s }' % (ln + 1, ', '.join(regs),
                                         put(fn, rd, q, 'vtbl(t, %d, %s, %s)' % (ln + 1, fn.v(rm), dflt)))


# ----------------------------------------------------------------------------- crypto
def crypto_aes(fn, w):
    opcode, rn, rd = bits(w, 16, 12), bits(w, 9, 5), bits(w, 4, 0)
    if bits(w, 23, 22):
        raise Unimpl('aes-size')
    if opcode in (4, 5):
        return fn.setv(rd, '%s(%s, %s)' % ('aese' if opcode == 4 else 'aesd', fn.v(rd), fn.v(rn)))
    if opcode in (6, 7):
        return fn.setv(rd, '%s(%s)' % ('aesmc' if opcode == 6 else 'aesimc', fn.v(rn)))
    raise Unimpl('aes')


def crypto_sha3(fn, w):
    opcode, rm, rn, rd = bits(w, 14, 12), bits(w, 20, 16), bits(w, 9, 5), bits(w, 4, 0)
    if bits(w, 23, 22):
        raise Unimpl('sha3-size')
    name = ['sha1c', 'sha1p', 'sha1m', 'sha1su0', 'sha256h', 'sha256h2', 'sha256su1'][opcode] if opcode < 7 else None
    if name is None:
        raise Unimpl('sha3')
    return fn.setv(rd, '%s(%s, %s, %s)' % (name, fn.v(rd), fn.v(rn), fn.v(rm)))


def crypto_sha2(fn, w):
    opcode, rn, rd = bits(w, 16, 12), bits(w, 9, 5), bits(w, 4, 0)
    if opcode == 0:
        return fn.setv(rd, 'vz32(sha1h(((v4u32)%s)[0]))' % fn.v(rn))
    if opcode in (1, 2):
        return fn.setv(rd, '%s(%s, %s)' % ('sha1su1' if opcode == 1 else 'sha256su0', fn.v(rd), fn.v(rn)))
    raise Unimpl('sha2')


# ----------------------------------------------------------------------------- structure loads and stores
def ldst_struct(fn, w, pc):
    q, l, post = bits(w, 30, 30), bits(w, 22, 22), bits(w, 23, 23)
    rm, opcode, size, rn, rt = bits(w, 20, 16), bits(w, 15, 12), bits(w, 11, 10), bits(w, 9, 5), bits(w, 4, 0)
    single = bits(w, 24, 24)
    stmts = ['u64 a = %s;' % fn.x(rn, sp=True)]
    if not single:
        if bits(w, 21, 21):
            raise Unimpl('ldst-multi')
        layout = {0x0: (4, 4), 0x2: (1, 4), 0x4: (3, 3), 0x6: (1, 3), 0x7: (1, 1), 0x8: (2, 2), 0xa: (1, 2)}.get(opcode)
        if layout is None:
            raise Unimpl('ldst-multi-op')
        selem, rpt = layout
        if size == 3 and not q and selem > 1:
            raise Unimpl('ldst-1d')
        nbytes = 16 if q else 8
        total = nbytes * rpt * (selem if selem > 1 else 1) if selem > 1 else nbytes * rpt
        if selem == 1:                                                # LD1/ST1 of 1..4 registers
            for r in range(rpt):
                reg = (rt + r) % 32
                off = 'a + %d' % (r * nbytes) if r else 'a'
                if l:
                    stmts.append(fn.setv(reg, 'LDQ(%s)' % off) if q else fn.setv(reg, 'vz64(LD64(%s))' % off))
                else:
                    stmts.append('STQ(%s, %s);' % (off, fn.v(reg)) if q else 'ST64(%s, %s[0]);' % (off, fn.v(reg)))
            total = nbytes * rpt
        else:                                                         # LD2/3/4, ST2/3/4: interleaved elements
            esz = 1 << size
            lanes = nbytes // esz
            regs = [(rt + i) % 32 for i in range(selem)]
            t = vt(size, 'u')
            if l:
                stmts.append('%s %s;' % (t, ', '.join('t%d = {0}' % i for i in range(selem))))
                for e in range(lanes):
                    for s in range(selem):
                        stmts.append('t%d[%d] = %s(a + %d);' % (s, e, LD[size], (e * selem + s) * esz))
                for s in range(selem):
                    stmts.append(fn.setv(regs[s], '(v128)t%d' % s))
            else:
                for s in range(selem):
                    stmts.append('%s t%d = (%s)%s;' % (t, s, t, fn.v(regs[s])))
                for e in range(lanes):
                    for s in range(selem):
                        stmts.append('%s(a + %d, t%d[%d]);' % (ST[size], (e * selem + s) * esz, s, e))
            total = nbytes * selem
    else:                                                             # single structure (one lane, or replicate)
        r = bits(w, 21, 21)
        s_ = bits(w, 12, 12)
        op3 = opcode >> 1
        selem = (bits(w, 13, 13) << 1 | r) + 1
        if op3 == 6:                                                  # LD1R..LD4R
            if not l:
                raise Unimpl('st-r')
            esz = 1 << size
            lanes = (16 if q else 8) // esz
            for i in range(selem):
                reg = (rt + i) % 32
                e = '(%s)%s(a + %d)' % ('u%d' % (8 << size), LD[size], i * esz)
                stmts.append(put(fn, reg, q, splat(vt(size), e, 16 >> size)))
            total = esz * selem
        else:
            if op3 == 0:
                sz, idx = 0, (q << 3) | (s_ << 2) | size
            elif op3 == 2 and not size & 1:
                sz, idx = 1, (q << 2) | (s_ << 1) | (size >> 1)
            elif op3 == 4 and size == 0:
                sz, idx = 2, (q << 1) | s_
            elif op3 == 4 and size == 1 and not s_:
                sz, idx = 3, q
            else:
                raise Unimpl('ldst-single')
            esz = 1 << sz
            t = vt(sz)
            for i in range(selem):
                reg = (rt + i) % 32
                if l:
                    stmts.append('{ %s t = (%s)%s; t[%d] = %s(a + %d); %s }' % (t, t, fn.v(reg), idx, LD[sz], i * esz,
                                                                              fn.setv(reg, '(v128)t')))
                else:
                    stmts.append('%s(a + %d, ((%s)%s)[%d]);' % (ST[sz], i * esz, t, fn.v(reg), idx))
            total = esz * selem
    if post:
        inc = hx(total) if rm == 31 else fn.x(rm)
        stmts.append(fn.setx(rn, 'a + %s' % inc, sp=True))
    return '{ ' + ' '.join(stmts) + ' }'
