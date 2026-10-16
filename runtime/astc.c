/* ASTC LDR decoding (the GTX 1650 has no ASTC sampling): 128-bit blocks -> RGBA8, written from the Khronos
 * ASTC specification. HDR endpoint modes, 3D blocks and reserved encodings decode to the error colour
 * (magenta), as the specification asks. */
#include "astc.h"
#include <string.h>

static u32 getbits(const u8 *b, u32 pos, u32 n) {
    u32 v = 0;
    for (u32 i = 0; i < n; i++) v |= (u32)(b[(pos + i) >> 3] >> ((pos + i) & 7) & 1) << i;
    return v;
}

/* ---------------------------------------------------------------------------------------------- ISE */
/* quantisation levels: index -> {bits, trits, quints} */
static const u8 quant[21][3] = {
    {1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {0, 0, 1}, {1, 1, 0}, {3, 0, 0}, {1, 0, 1}, {2, 1, 0}, {4, 0, 0}, {2, 0, 1},
    {3, 1, 0}, {5, 0, 0}, {3, 0, 1}, {4, 1, 0}, {6, 0, 0}, {4, 0, 1}, {5, 1, 0}, {7, 0, 0}, {5, 0, 1}, {6, 1, 0},
    {8, 0, 0},
};   /* ranges 2 3 4 5 6 8 10 12 16 20 24 32 40 48 64 80 96 128 160 192 256 */

static u32 ise_bits(u32 n, u32 q) {
    u32 b = quant[q][0];
    if (quant[q][1]) return b * n + (8 * n + 4) / 5;
    if (quant[q][2]) return b * n + (7 * n + 2) / 3;
    return b * n;
}

static void decode_trits(u32 t, u8 out[5]) {
    u32 c, t4, t3;
    if ((t >> 2 & 7) == 7) {
        c = (t >> 5 & 7) << 2 | (t & 3);
        t4 = t3 = 2;
    } else {
        c = t & 31;
        if ((t >> 5 & 3) == 3) { t4 = 2; t3 = t >> 7 & 1; }
        else { t4 = t >> 7 & 1; t3 = t >> 5 & 3; }
    }
    u32 t2, t1, t0;
    if ((c & 3) == 3) {
        t2 = 2; t1 = c >> 4 & 1;
        t0 = (c >> 3 & 1) << 1 | ((c >> 2 & 1) & ~(c >> 3 & 1));
    } else if ((c >> 2 & 3) == 3) {
        t2 = t1 = 2; t0 = c & 3;
    } else {
        t2 = c >> 4 & 1; t1 = c >> 2 & 3;
        t0 = (c >> 1 & 1) << 1 | ((c & 1) & ~(c >> 1 & 1));
    }
    out[0] = (u8)t0; out[1] = (u8)t1; out[2] = (u8)t2; out[3] = (u8)t3; out[4] = (u8)t4;
}

static void decode_quints(u32 q, u8 out[3]) {
    u32 q0, q1, q2;
    if ((q >> 1 & 3) == 3 && (q >> 5 & 3) == 0) {
        u32 b0 = q & 1;
        q2 = b0 << 2 | ((q >> 4 & 1) & ~b0) << 1 | ((q >> 3 & 1) & ~b0);
        q1 = q0 = 4;
    } else {
        u32 c;
        if ((q >> 1 & 3) == 3) {
            q2 = 4;
            c = (q >> 3 & 3) << 3 | ((~q >> 5) & 3) << 1 | (q & 1);
        } else {
            q2 = q >> 5 & 3;
            c = q & 31;
        }
        if ((c & 7) == 5) { q1 = 4; q0 = c >> 3 & 3; }
        else { q1 = c >> 3 & 3; q0 = c & 7; }
    }
    out[0] = (u8)q0; out[1] = (u8)q1; out[2] = (u8)q2;
}

/* n values of quantisation q from bit pos: value (D << bits | m) split as tq[] (trit/quint) and m[] (bits).
 * Bits past the sequence's own length read as zero (they may belong to other fields). */
static void ise_decode_raw(const u8 *b, u32 pos, u32 n, u32 q, u8 *tq, u16 *m);
static void ise_decode(const u8 *src, u32 start, u32 n, u32 q, u8 *tq, u16 *m) {
    u8 b[24] = {0};
    u32 total = ise_bits(n, q);
    for (u32 i = 0; i < total && start + i < 128; i++)
        b[i >> 3] |= (u8)((src[(start + i) >> 3] >> ((start + i) & 7) & 1) << (i & 7));
    ise_decode_raw(b, 0, n, q, tq, m);
}
static void ise_decode_raw(const u8 *b, u32 pos, u32 n, u32 q, u8 *tq, u16 *m) {
    u32 nb = quant[q][0];
    if (quant[q][1]) {
        for (u32 i = 0; i < n; i += 5) {
            u32 t = 0;
            static const u8 tbits[5] = {2, 2, 1, 2, 1}, tpos[5] = {0, 2, 4, 5, 7};
            u8 tr[5];
            for (u32 k = 0; k < 5; k++) {
                if (i + k < n) m[i + k] = (u16)getbits(b, pos, nb);
                pos += nb;
                t |= getbits(b, pos, tbits[k]) << tpos[k];
                pos += tbits[k];
            }
            decode_trits(t, tr);
            for (u32 k = 0; k < 5 && i + k < n; k++) tq[i + k] = tr[k];
        }
    } else if (quant[q][2]) {
        for (u32 i = 0; i < n; i += 3) {
            u32 qv = 0;
            static const u8 qbits[3] = {3, 2, 2}, qpos[3] = {0, 3, 5};
            u8 qu[3];
            for (u32 k = 0; k < 3; k++) {
                if (i + k < n) m[i + k] = (u16)getbits(b, pos, nb);
                pos += nb;
                qv |= getbits(b, pos, qbits[k]) << qpos[k];
                pos += qbits[k];
            }
            decode_quints(qv, qu);
            for (u32 k = 0; k < 3 && i + k < n; k++) tq[i + k] = qu[k];
        }
    } else {
        for (u32 i = 0; i < n; i++) {
            m[i] = (u16)getbits(b, pos, nb);
            tq[i] = 0;
            pos += nb;
        }
    }
}

static u32 replicate(u32 v, u32 from, u32 to) {
    if (!from) return 0;
    u32 r = 0;
    int have = 0;
    while (have < (int)to) {
        r = r << from | v;
        have += from;
    }
    return r >> (have - to);
}

/* colour values -> 0..255 */
static u32 unq_color(u32 q, u32 d, u32 mv) {
    u32 nb = quant[q][0];
    if (!quant[q][1] && !quant[q][2]) return replicate(mv, nb, 8);
    if (nb == 0) {
        if (quant[q][1]) return d * 128 > 255 ? 255 : d * 128;      /* 0 128 255 */
        return d == 4 ? 255 : d * 64;                               /* 0 64 128 192 255 */
    }
    u32 a = mv & 1 ? 0x1ff : 0, bb = mv >> 1 & 1, c = mv >> 2 & 1, dd = mv >> 3 & 1, e = mv >> 4 & 1, f = mv >> 5 & 1;
    u32 B = 0, C = 0;
    if (quant[q][1]) {
        switch (nb) {
        case 1: C = 204; break;
        case 2: C = 93; B = bb << 8 | bb << 4 | bb << 2 | bb << 1; break;
        case 3: C = 44; B = c << 8 | bb << 7 | c << 3 | bb << 2 | c << 1 | bb; break;
        case 4: C = 22; B = dd << 8 | c << 7 | bb << 6 | dd << 2 | c << 1 | bb; break;
        case 5: C = 11; B = e << 8 | dd << 7 | c << 6 | bb << 5 | e << 1 | dd; break;
        case 6: C = 5; B = f << 8 | e << 7 | dd << 6 | c << 5 | bb << 4 | f; break;
        }
    } else {
        switch (nb) {
        case 1: C = 113; break;
        case 2: C = 54; B = bb << 8 | bb << 3 | bb << 2; break;
        case 3: C = 26; B = c << 8 | bb << 7 | c << 2 | bb << 1 | c; break;
        case 4: C = 13; B = dd << 8 | c << 7 | bb << 6 | dd << 1 | c; break;
        case 5: C = 6; B = e << 8 | dd << 7 | c << 6 | bb << 5 | e; break;
        }
    }
    u32 t = d * C + B;
    t ^= a;
    return (a & 0x80) | t >> 2;
}

/* weights -> 0..64 */
static u32 unq_weight(u32 q, u32 d, u32 mv) {
    u32 nb = quant[q][0], r;
    if (!quant[q][1] && !quant[q][2]) {
        r = replicate(mv, nb, 6);
    } else if (nb == 0) {
        r = quant[q][1] ? (u32[]){0, 32, 63}[d] : (u32[]){0, 16, 32, 47, 63}[d];
    } else {
        u32 a = mv & 1 ? 0x7f : 0, bb = mv >> 1 & 1, c = mv >> 2 & 1, B = 0, C = 0;
        if (quant[q][1]) {
            if (nb == 1) C = 50;
            else if (nb == 2) { C = 23; B = bb << 6 | bb << 2 | bb; }
            else { C = 11; B = c << 6 | bb << 5 | c << 1 | bb; }
        } else {
            if (nb == 1) C = 28;
            else { C = 13; B = bb << 6 | bb << 1; }
        }
        u32 t = d * C + B;
        t ^= a;
        r = (a & 0x20) | t >> 2;
    }
    return r > 32 ? r + 1 : r;
}

/* ---------------------------------------------------------------------------------------------- endpoints */
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void bit_transfer_signed(int *a, int *b) {
    *b = (*b >> 1) | (*a & 0x80);
    *a = (*a >> 1) & 0x3f;
    if (*a & 0x20) *a -= 0x40;
}

static void blue_contract(int *c) { c[0] = (c[0] + c[2]) >> 1; c[1] = (c[1] + c[2]) >> 1; }

/* false for HDR modes */
static bool endpoints(u32 cem, const int *v, int e0[4], int e1[4]) {
    int a, b, c, d;
    switch (cem) {
    case 0:
        e0[0] = e0[1] = e0[2] = v[0]; e0[3] = 255;
        e1[0] = e1[1] = e1[2] = v[1]; e1[3] = 255;
        return true;
    case 1: {
        int l0 = (v[0] >> 2) | (v[1] & 0xc0), l1 = l0 + (v[1] & 0x3f);
        if (l1 > 255) l1 = 255;
        e0[0] = e0[1] = e0[2] = l0; e0[3] = 255;
        e1[0] = e1[1] = e1[2] = l1; e1[3] = 255;
        return true;
    }
    case 4:
        e0[0] = e0[1] = e0[2] = v[0]; e0[3] = v[2];
        e1[0] = e1[1] = e1[2] = v[1]; e1[3] = v[3];
        return true;
    case 5:
        a = v[1]; b = v[0]; bit_transfer_signed(&a, &b);
        c = v[3]; d = v[2]; bit_transfer_signed(&c, &d);
        e0[0] = e0[1] = e0[2] = b; e0[3] = d;
        e1[0] = e1[1] = e1[2] = clamp255(a + b); e1[3] = clamp255(c + d);
        return true;
    case 6:
        e0[0] = v[0] * v[3] >> 8; e0[1] = v[1] * v[3] >> 8; e0[2] = v[2] * v[3] >> 8; e0[3] = 255;
        e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = 255;
        return true;
    case 8: case 12: {
        int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
        int a0 = cem == 12 ? v[6] : 255, a1 = cem == 12 ? v[7] : 255;
        if (s1 >= s0) {
            e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e0[3] = a0;
            e1[0] = v[1]; e1[1] = v[3]; e1[2] = v[5]; e1[3] = a1;
        } else {
            e0[0] = v[1]; e0[1] = v[3]; e0[2] = v[5]; e0[3] = a1;
            e1[0] = v[0]; e1[1] = v[2]; e1[2] = v[4]; e1[3] = a0;
            blue_contract(e0);
            blue_contract(e1);
        }
        return true;
    }
    case 9: case 13: {
        int o[4], bs[4];
        for (int i = 0; i < 4; i++) {
            o[i] = i < 3 || cem == 13 ? v[2 * i + 1] : 0;
            bs[i] = i < 3 || cem == 13 ? v[2 * i] : 255;
            if (i < 3 || cem == 13) bit_transfer_signed(&o[i], &bs[i]);
        }
        if (o[0] + o[1] + o[2] >= 0) {
            for (int i = 0; i < 4; i++) { e0[i] = bs[i]; e1[i] = clamp255(bs[i] + o[i]); }
        } else {
            for (int i = 0; i < 4; i++) { e0[i] = clamp255(bs[i] + o[i]); e1[i] = bs[i]; }
            blue_contract(e0);
            blue_contract(e1);
        }
        if (cem == 9) e0[3] = e1[3] = 255;
        return true;
    }
    case 10:
        e0[0] = v[0] * v[3] >> 8; e0[1] = v[1] * v[3] >> 8; e0[2] = v[2] * v[3] >> 8; e0[3] = v[4];
        e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = v[5];
        return true;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------------------------------- partitions */
static u32 hash52(u32 p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4; p ^= p >> 5;
    p += p << 16; p ^= p >> 7; p ^= p >> 3; p ^= p << 6; p ^= p >> 17;
    return p;
}

static u32 select_partition(u32 seed, u32 x, u32 y, u32 count, bool small) {
    if (small) { x <<= 1; y <<= 1; }
    seed += (count - 1) * 1024;
    u32 r = hash52(seed);
    u32 s[12];
    s[0] = r & 15; s[1] = r >> 4 & 15; s[2] = r >> 8 & 15; s[3] = r >> 12 & 15; s[4] = r >> 16 & 15;
    s[5] = r >> 20 & 15; s[6] = r >> 24 & 15; s[7] = r >> 28 & 15; s[8] = r >> 18 & 15; s[9] = r >> 22 & 15;
    s[10] = r >> 26 & 15; s[11] = (r >> 30 | r << 2) & 15;
    for (int i = 0; i < 12; i++) s[i] *= s[i];
    u32 sh1, sh2;
    if (seed & 1) { sh1 = seed & 2 ? 4 : 5; sh2 = count == 3 ? 6 : 5; }
    else { sh1 = count == 3 ? 6 : 5; sh2 = seed & 2 ? 4 : 5; }
    u32 sh3 = seed & 16 ? sh1 : sh2;
    s[0] >>= sh1; s[1] >>= sh2; s[2] >>= sh1; s[3] >>= sh2; s[4] >>= sh1; s[5] >>= sh2; s[6] >>= sh1; s[7] >>= sh2;
    s[8] >>= sh3; s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3;
    u32 a = (s[0] * x + s[1] * y + (r >> 14)) & 63;
    u32 b = (s[2] * x + s[3] * y + (r >> 10)) & 63;
    u32 c = (s[4] * x + s[5] * y + (r >> 6)) & 63;
    u32 d = (s[6] * x + s[7] * y + (r >> 2)) & 63;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

/* ---------------------------------------------------------------------------------------------- blocks */
static void error_block(u8 *out, u32 bw, u32 bh, u32 pitch) {
    for (u32 y = 0; y < bh; y++)
        for (u32 x = 0; x < bw; x++) {
            u8 *p = out + y * pitch + x * 4;
            p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255;
        }
}

static bool block_mode(u32 mode, u32 *wx, u32 *wy, bool *dual, u32 *wq) {
    u32 r = mode >> 4 & 1, h = mode >> 9 & 1, d = mode >> 10 & 1, a = mode >> 5 & 3, x, y;
    if (mode & 3) {
        r |= (mode & 3) << 1;
        u32 b = mode >> 7 & 3;
        switch (mode >> 2 & 3) {
        case 0: x = b + 4; y = a + 2; break;
        case 1: x = b + 8; y = a + 2; break;
        case 2: x = a + 2; y = b + 8; break;
        default:
            b &= 1;
            if (mode & 0x100) { x = b + 2; y = a + 2; }
            else { x = a + 2; y = b + 6; }
        }
    } else {
        r |= (mode >> 2 & 3) << 1;
        if ((mode >> 2 & 3) == 0) return false;
        u32 b = mode >> 9 & 3;
        switch (mode >> 7 & 3) {
        case 0: x = 12; y = a + 2; break;
        case 1: x = a + 2; y = 12; break;
        case 2: x = a + 6; y = b + 6; d = 0; h = 0; break;
        default:
            if ((mode >> 5 & 3) == 0) { x = 6; y = 10; }
            else if ((mode >> 5 & 3) == 1) { x = 10; y = 6; }
            else return false;
        }
    }
    if (r < 2) return false;
    *wx = x; *wy = y; *dual = d;
    *wq = (r - 2) + 6 * h;
    /* the quantisation index into quant[] for weights: ranges 2 3 4 5 6 8 / 10 12 16 20 24 32 */
    static const u8 wmap[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    *wq = wmap[*wq];
    return true;
}

void astc_decode_block(const u8 *blk, u32 bw, u32 bh, bool srgb, u8 *out, u32 pitch) {
    u32 mode = getbits(blk, 0, 11);
    if ((mode & 0x1ff) == 0x1fc) {      /* void extent: one colour */
        if (mode & 0x200) { error_block(out, bw, bh, pitch); return; }
        u8 c[4];
        for (int i = 0; i < 4; i++) c[i] = (u8)(getbits(blk, 64 + 16 * i, 16) >> 8);
        for (u32 y = 0; y < bh; y++)
            for (u32 x = 0; x < bw; x++) memcpy(out + y * pitch + x * 4, c, 4);
        return;
    }
    u32 wx, wy, wq;
    bool dual;
    if (!block_mode(mode, &wx, &wy, &dual, &wq) || wx > bw || wy > bh) { error_block(out, bw, bh, pitch); return; }
    u32 nweights = wx * wy * (dual ? 2 : 1);
    u32 wbits = ise_bits(nweights, wq);
    if (nweights > 64 || wbits < 24 || wbits > 96) { error_block(out, bw, bh, pitch); return; }
    u32 parts = getbits(blk, 11, 2) + 1;
    if (dual && parts == 4) { error_block(out, bw, bh, pitch); return; }
    u32 cem[4], color_start, extra = 0, seed = 0;
    u32 below = 128 - wbits;            /* weights occupy the top bits */
    if (parts == 1) {
        cem[0] = getbits(blk, 13, 4);
        color_start = 17;
    } else {
        seed = getbits(blk, 13, 10);
        u32 cb = getbits(blk, 23, 6);
        color_start = 29;
        if ((cb & 3) == 0) {
            for (u32 i = 0; i < parts; i++) cem[i] = cb >> 2;
        } else {
            extra = 3 * parts - 4;
            u32 enc = cb >> 2 | getbits(blk, below - extra, extra) << 4;
            u32 base = (cb & 3) - 1;
            for (u32 i = 0; i < parts; i++) {
                u32 ci = enc >> i & 1, mi = enc >> (parts + 2 * i) & 3;
                cem[i] = (base + ci) << 2 | mi;
            }
        }
    }
    u32 color_end = below - extra - (dual ? 2 : 0);
    u32 plane2_comp = dual ? getbits(blk, color_end, 2) : 0;
    u32 nvals = 0;
    for (u32 i = 0; i < parts; i++) nvals += ((cem[i] >> 2) + 1) * 2;
    if (nvals > 18 || color_end <= color_start) { error_block(out, bw, bh, pitch); return; }
    u32 avail = color_end - color_start;
    int cq = -1;
    for (int q = 20; q >= 0; q--)
        if (ise_bits(nvals, (u32)q) <= avail) { cq = q; break; }
    if (cq < 4) { error_block(out, bw, bh, pitch); return; }    /* the spec needs at least range 6 */
    u8 tq[64];
    u16 mv[64];
    ise_decode(blk, color_start, nvals, (u32)cq, tq, mv);
    int vals[18];
    for (u32 i = 0; i < nvals; i++) vals[i] = (int)unq_color((u32)cq, tq[i], mv[i]);
    int e0[4][4], e1[4][4];
    u32 vi = 0;
    for (u32 i = 0; i < parts; i++) {
        if (!endpoints(cem[i], vals + vi, e0[i], e1[i])) { error_block(out, bw, bh, pitch); return; }
        vi += ((cem[i] >> 2) + 1) * 2;
    }
    /* weights: stored bit-reversed from the top of the block */
    u8 rev[16];
    for (int i = 0; i < 16; i++) {
        u8 v = blk[15 - i], r = 0;
        for (int k = 0; k < 8; k++) r |= (u8)((v >> k & 1) << (7 - k));
        rev[i] = r;
    }
    ise_decode(rev, 0, nweights, wq, tq, mv);
    u8 w[64];
    for (u32 i = 0; i < nweights; i++) w[i] = (u8)unq_weight(wq, tq[i], mv[i]);
    u32 ds = (1024 + bw / 2) / (bw - 1 ? bw - 1 : 1), dt = (1024 + bh / 2) / (bh - 1 ? bh - 1 : 1);
    bool small = bw * bh < 31;
    u32 planes = dual ? 2 : 1;
    for (u32 y = 0; y < bh; y++)
        for (u32 x = 0; x < bw; x++) {
            u32 part = parts > 1 ? select_partition(seed, x, y, parts, small) : 0;
            /* infill */
            u32 gs = (ds * x * (wx - 1) + 32) >> 6, gt = (dt * y * (wy - 1) + 32) >> 6;
            u32 js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            u32 w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            u32 tw[2];
            for (u32 pl = 0; pl < planes; pl++) {
                u32 v0 = js + jt * wx;
                u32 p00 = w[v0 * planes + pl];
                u32 p01 = js + 1 < wx ? w[(v0 + 1) * planes + pl] : 0;
                u32 p10 = jt + 1 < wy ? w[(v0 + wx) * planes + pl] : 0;
                u32 p11 = js + 1 < wx && jt + 1 < wy ? w[(v0 + wx + 1) * planes + pl] : 0;
                tw[pl] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
            }
            u8 *o = out + y * pitch + x * 4;
            for (u32 c = 0; c < 4; c++) {
                u32 wt = dual && c == plane2_comp ? tw[1] : tw[0];
                u32 c0 = (u32)e0[part][c], c1 = (u32)e1[part][c];
                c0 = srgb ? (c0 << 8 | 0x80) : (c0 << 8 | c0);
                c1 = srgb ? (c1 << 8 | 0x80) : (c1 << 8 | c1);
                u32 v = (c0 * (64 - wt) + c1 * wt + 32) >> 6;
                o[c] = (u8)(v >> 8);
            }
        }
}

void astc_decode(const u8 *blocks, u32 wblocks, u32 hblocks, u32 bw, u32 bh, bool srgb, u8 *out, u32 width,
                 u32 height) {
    u8 tmp[12 * 12 * 4];
    for (u32 by = 0; by < hblocks; by++)
        for (u32 bx = 0; bx < wblocks; bx++) {
            astc_decode_block(blocks + 16 * ((u64)by * wblocks + bx), bw, bh, srgb, tmp, bw * 4);
            for (u32 y = 0; y < bh && by * bh + y < height; y++) {
                u32 n = bx * bw + bw <= width ? bw : width - bx * bw;
                memcpy(out + ((u64)(by * bh + y) * width + bx * bw) * 4, tmp + y * bw * 4, n * 4);
            }
        }
}
