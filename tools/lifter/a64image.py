"""Module images for the runtime: what rtld does on the console, done once at build time.

The modules live at fixed addresses (a64lift.BASES), so every relocation can be applied here: RELATIVE, and
GLOB_DAT / JUMP_SLOT / ABS64 against the first definition in load order main -> subsdk0 -> sdk (the order rtld
searches, and the one a64lift.py uses to resolve calls through PLT stubs). Unresolved symbols are all weak and
become 0 (+ addend), as rtld leaves them.

Output, one file per module, <outdir>/<module>.img (little-endian):
    ImgHeader (see runtime/hos.h): magic 'MKIM', version, base, segment bounds, DT_INIT / DT_FINI,
    symbol count, string table size;
    nsym x {u64 addr; u32 name; u32 size}   exported symbols (absolute addresses), sorted by address
    string table
    image bytes: text, ro, data up to the end of initialised data (bss is zero-filled by the runtime)

Usage: python a64image.py <outdir>      (e.g. ../../../build/image)
"""
import os
import struct
import sys

from a64lift import MODULES, Program

MAGIC, VERSION = 0x4d494b4d, 1        # 'MKIM'
HDR = struct.Struct('<II' + 'Q' * 10 + 'II')
R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027


def build(prog, name):
    mod = prog.mods[name]
    m, base = mod.m, mod.base
    t, ro, data = m.segs[0], m.segs[1], m.segs[2]
    file_end = data[1] + data[2]                       # end of initialised data
    img = bytearray(m.bytes(0, file_end))
    unresolved = set()
    for off, typ, si, add in m.rels:
        if typ == R_RELATIVE:
            val = base + add
        elif typ in (R_ABS64, R_GLOB_DAT, R_JUMP_SLOT):
            sname, value, size, info, shndx = m.syms[si]
            if shndx != 0 and info >> 4 == 0:          # local symbol: this module's own definition
                val = base + value + add
            elif sname in prog.sym_addr:
                val = prog.sym_addr[sname] + add
            else:
                if info >> 4 != 2:
                    raise SystemExit('%s: strong symbol %s is unresolved' % (name, sname))
                unresolved.add(sname)
                val = add
        else:
            raise SystemExit('%s: relocation type %d at %x' % (name, typ, off))
        struct.pack_into('<Q', img, off, val & 0xffffffffffffffff)
    syms = sorted((base + v, s, size) for s, v, size, info, shndx in m.exports() if v)
    strtab = bytearray()
    symtab = bytearray()
    for addr, s, size in syms:
        symtab += struct.pack('<QII', addr, len(strtab), min(size, 0xffffffff))
        strtab += s.encode() + b'\0'
    while len(strtab) % 8:
        strtab += b'\0'
    dt_init = base + m.tag('INIT') if m.tags.get(12) else 0
    dt_fini = base + m.tag('FINI') if m.tags.get(13) else 0
    hdr = HDR.pack(MAGIC, VERSION, base, base + t[1] + t[2], base + ro[1], base + ro[1] + ro[2],
                   base + data[1], base + file_end, base + data[1] + data[3], dt_init, dt_fini, 0,
                   len(syms), len(strtab))
    return hdr + bytes(symtab) + bytes(strtab) + bytes(img), unresolved


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    prog = Program()
    for n in MODULES:
        data, unresolved = build(prog, n)
        path = os.path.join(out, n + '.img')
        try:
            same = open(path, 'rb').read() == data
        except OSError:
            same = False
        if not same:
            with open(path, 'wb') as f:
                f.write(data)
        print('%-8s %10d bytes, weak unresolved: %s' % (n, len(data), ', '.join(sorted(unresolved)) or '-'))


if __name__ == '__main__':
    main()
