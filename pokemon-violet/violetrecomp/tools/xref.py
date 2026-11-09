"""Code references (ADRP + ADD / LDR pairs) to addresses or strings in main.elf (dump/elf). Game-derived output.

    python tools/xref.py <dump/elf/main.elf> <va|"string">...     (va hex; a string is looked up in the data first)
"""
import struct
import sys


def load(path):
    d = open(path, 'rb').read()
    phoff, phn = struct.unpack_from('<Q', d, 0x20)[0], struct.unpack_from('<H', d, 0x38)[0]
    segs = []
    for i in range(phn):
        t, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + 56 * i)
        if t == 1:
            segs.append((va, off, fs, fl))
    return d, segs


def refs(d, segs, targets):
    va0, off, fs, _ = [s for s in segs if s[3] & 1][0]
    w = struct.unpack_from('<%dI' % (fs // 4), d, off)
    out = {t: [] for t in targets}
    pages = {}
    for t in targets:
        pages.setdefault(t & ~0xfff, []).append(t)
    for i, x in enumerate(w):
        if (x & 0x9f000000) != 0x90000000:
            continue
        rd = x & 31
        imm = ((x >> 29) & 3) | (((x >> 5) & 0x7ffff) << 2)
        if imm & (1 << 20):
            imm -= 1 << 21
        pc = va0 + 4 * i
        page = (pc & ~0xfff) + (imm << 12)
        if page not in pages:
            continue
        for j in range(i + 1, min(i + 8, len(w))):
            y = w[j]
            if (y & 0xff800000) in (0x91000000,) and ((y >> 5) & 31) == rd:     # ADD Xd, Xn, #imm
                a = page + ((y >> 10) & 0xfff)
                if a in out:
                    out[a].append(va0 + 4 * j)
                break
    return out


def main():
    d, segs = load(sys.argv[1])
    targets = []
    for a in sys.argv[2:]:
        try:
            targets.append(int(a, 16))
        except ValueError:
            o = d.find(a.encode() + b'\0')
            for va, off, fs, _ in segs:
                if off <= o < off + fs:
                    targets.append(va + o - off)
                    print('%r at %#x' % (a, va + o - off))
    for t, r in refs(d, segs, targets).items():
        print('%#x: %s' % (t, ' '.join('%#x' % x for x in r)))


if __name__ == '__main__':
    main()
