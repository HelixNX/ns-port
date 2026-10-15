"""NSO -> ELF, for both 64-bit (AArch64) and 32-bit (AArch32) modules.

Based on ssbu-recomp tools/extract/nso2elf.py (GPLv3), extended for the 32-bit modules of MK8DX:
the module class is taken from the dynamic section's DT_SYMENT (24 = Elf64_Sym, 16 = Elf32_Sym).

Usage: python nso2elf.py <in.nso> <out.elf>
"""
import hashlib
import struct
import sys

import lz4.block

DT_NULL, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_STRSZ, DT_SYMENT = 0, 4, 5, 6, 10, 11
DT_GNU_HASH = 0x6ffffef5


def load_nso(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'NSO0', 'not an NSO'
    flags = struct.unpack_from('<I', d, 12)[0]
    segs = []
    for i, (ho, cso) in enumerate(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68))):
        foff, moff, size = struct.unpack_from('<III', d, ho)
        csz = struct.unpack_from('<I', d, cso)[0]
        raw = d[foff:foff + csz]
        if flags & (1 << i):
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        assert len(raw) == size
        if hashlib.sha256(raw).digest() != d[0xA0 + i * 0x20:0xC0 + i * 0x20]:
            raise ValueError('segment %d hash mismatch' % i)
        segs.append((moff, raw))
    bss = struct.unpack_from('<I', d, 0x3c)[0]
    return segs, bss, d[0x40:0x54].hex()


def read_dynamic(mem, dyn, is64):
    fmt, ent = ('<qQ', 16) if is64 else ('<iI', 8)
    tags, n = {}, 0
    while True:
        t, v = struct.unpack_from(fmt, mem, dyn + n * ent)
        n += 1
        if t == DT_NULL:
            break
        tags.setdefault(t, v)
    return tags, n * ent


def detect_class(mem, dyn):
    """32-bit modules use 8-byte Elf32_Dyn entries; find DT_SYMENT under both readings."""
    for is64 in (True, False):
        try:
            tags, _ = read_dynamic(mem, dyn, is64)
        except struct.error:
            continue
        if tags.get(DT_SYMENT) == (24 if is64 else 16):
            return is64
    raise ValueError('cannot tell the module class from the dynamic section')


def main(src, dst):
    segs, bss, build_id = load_nso(src)
    (tv, text), (rv, ro), (dv, data) = segs
    mem = bytearray(dv + len(data))
    for v, r in segs:
        mem[v:v + len(r)] = r
    mod = struct.unpack_from('<I', mem, 4)[0]
    assert mem[mod:mod + 4] == b'MOD0'
    dyn_off, bss_s, bss_e = struct.unpack_from('<iii', mem, mod + 4)
    dyn = mod + dyn_off
    is64 = detect_class(mem, dyn)
    tags, dynsz = read_dynamic(mem, dyn, is64)
    symtab, strtab, strsz = tags[DT_SYMTAB], tags[DT_STRTAB], tags[DT_STRSZ]
    syment = 24 if is64 else 16
    if DT_HASH in tags:
        nsym = struct.unpack_from('<I', mem, tags[DT_HASH] + 4)[0]
    else:
        nsym = (strtab - symtab) // syment
    print('%s  build id %s  %s  text %x ro %x data %x bss %x  dynsym %x (%d syms)' % (
        src, build_id, 'AArch64' if is64 else 'AArch32', len(text), len(ro), len(data), bss_e - bss_s, symtab, nsym))

    OFF = 0x1000
    names = bytearray(b'\0')
    secs = []
    shfmt = '<IIQQQQIIQQ' if is64 else '<IIIIIIIIII'
    shsize = 64 if is64 else 40

    def sec(name, typ, fl, addr, off, size, link=0, info=0, align=1, ent=0):
        ni = len(names)
        names.extend(name.encode() + b'\0')
        secs.append([ni, typ, fl, addr, off, size, link, info, align, ent])

    secs.append([0] * 10)
    sec('.text', 1, 6, tv, OFF + tv, len(text), align=16)
    if rv <= symtab and symtab + nsym * syment <= strtab <= rv + len(ro):
        sec('.rodata', 1, 2, rv, OFF + rv, symtab - rv, align=16)
        sec('.dynsym', 11, 2, symtab, OFF + symtab, nsym * syment, link=4, info=1, align=8, ent=syment)
        sec('.dynstr', 3, 2, strtab, OFF + strtab, strsz)
        r1 = strtab + strsz
        sec('.rodata.1', 1, 2, r1, OFF + r1, rv + len(ro) - r1)
    else:                                             # dynsym/dynstr not laid out as expected: keep .rodata whole
        sec('.rodata', 1, 2, rv, OFF + rv, len(ro), align=16)
    if dv <= dyn < dv + len(data):
        sec('.data', 1, 3, dv, OFF + dv, dyn - dv, align=16)
        sec('.dynamic', 6, 3, dyn, OFF + dyn, dynsz, link=4, align=8, ent=16 if is64 else 8)
        d1 = dyn + dynsz
        sec('.data.1', 1, 3, d1, OFF + d1, dv + len(data) - d1)
    else:
        sec('.data', 1, 3, dv, OFF + dv, len(data), align=16)
    sec('.bss', 8, 3, bss_s, OFF + dv + len(data), bss_e - bss_s, align=16)
    shstr_i = len(secs)
    shstr_off = OFF + len(mem)
    sec('.shstrtab', 3, 0, 0, shstr_off, 0)
    secs[-1][5] = len(names)
    shoff = (shstr_off + len(names) + 7) & ~7

    def phdr(t, fl, v, fsz, msz, al):
        if is64:
            return struct.pack('<IIQQQQQQ', t, fl, OFF + v, v, v, fsz, msz, al)
        return struct.pack('<IIIIIIII', t, OFF + v, v, v, fsz, msz, fl, al)

    ph = phdr(1, 5, tv, len(text), len(text), 0x1000)
    ph += phdr(1, 4, rv, len(ro), len(ro), 0x1000)
    ph += phdr(1, 6, dv, len(data), max(len(data), bss_e - dv, len(data) + bss), 0x1000)
    ph += phdr(2, 6, dyn, dynsz, dynsz, 8)
    if is64:
        ident = b'\x7fELF\x02\x01\x01' + bytes(9)
        eh = ident + struct.pack('<HHIQQQIHHHHHH', 3, 183, 1, 0, 0x40, shoff, 0, 0x40, 0x38, 4, shsize, len(secs), shstr_i)
    else:
        ident = b'\x7fELF\x01\x01\x01' + bytes(9)
        # EM_ARM, EABI version 5; entry 0 (the module start is the branch at text+0)
        eh = ident + struct.pack('<HHIIIIIHHHHHH', 3, 40, 1, 0, 0x34, shoff, 0x05000000, 0x34, 0x20, 4, shsize, len(secs), shstr_i)
    out = bytearray(eh + ph)
    out += bytes(OFF - len(out))
    out += mem
    out += names
    out += bytes(shoff - len(out))
    out += b''.join(struct.pack(shfmt, *s) for s in secs)
    open(dst, 'wb').write(out)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
