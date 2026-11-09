"""Reader for the 64-bit modules written by nso2elf.py: guest address a is file offset OFF + a.

Gives the segments, dynamic symbols, RELA relocations and the code addresses worth seeding IDA with
(exports, relocation targets inside .text, the module start).

Usage: python nxmod.py seeds <module.elf> <out.json>
       python nxmod.py info  <module.elf>
"""
import collections
import json
import struct
import sys

OFF = 0x1000
DT = dict(NULL=0, HASH=4, STRTAB=5, SYMTAB=6, RELA=7, RELASZ=8, STRSZ=10, SYMENT=11, INIT=12, FINI=13,
          JMPREL=23, PLTRELSZ=2, INIT_ARRAY=25, FINI_ARRAY=26, INIT_ARRAYSZ=27, FINI_ARRAYSZ=28)
R_AARCH64 = {257: 'ABS64', 1025: 'GLOB_DAT', 1026: 'JUMP_SLOT', 1027: 'RELATIVE', 1024: 'COPY'}


class Module:
    def __init__(self, path):
        self.path = path
        self.img = open(path, 'rb').read()
        assert self.img[4] == 2, 'expected a 64-bit ELF from nso2elf.py'
        # program headers: text, ro, data(+bss), dynamic
        phoff, = struct.unpack_from('<Q', self.img, 0x20)
        self.segs = []
        for i in range(4):
            t, fl, off, v, p, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', self.img, phoff + i * 0x38)
            self.segs.append((t, v, fsz, msz))
        self.text = (self.segs[0][1], self.segs[0][1] + self.segs[0][2])
        self.ro = (self.segs[1][1], self.segs[1][1] + self.segs[1][2])
        self.data = (self.segs[2][1], self.segs[2][1] + self.segs[2][3])   # includes bss
        self.size = self.data[1]
        dyn = self.segs[3][1]
        self.tags = collections.defaultdict(list)
        n = 0
        while True:
            t, v = struct.unpack_from('<qQ', self.img, OFF + dyn + n * 16)
            n += 1
            if t == 0:
                break
            self.tags[t].append(v)
        symtab, strtab = self.tag('SYMTAB'), self.tag('STRTAB')
        if self.tags.get(DT['HASH']):
            nsym = self.u32(self.tag('HASH') + 4)
        else:
            nsym = (strtab - symtab) // 24
        self.syms = []
        for i in range(nsym):
            name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', self.img, OFF + symtab + i * 24)
            end = self.img.index(b'\0', OFF + strtab + name)
            self.syms.append((self.img[OFF + strtab + name:end].decode('ascii', 'replace'), value, size, info, shndx))
        self.rels = []                     # (offset, type, symbol index, addend)
        for k_off, k_sz in (('RELA', 'RELASZ'), ('JMPREL', 'PLTRELSZ')):
            if self.tags.get(DT[k_off]):
                off, sz = self.tag(k_off), self.tag(k_sz)
                for i in range(sz // 24):
                    r_off, r_info, add = struct.unpack_from('<QQq', self.img, OFF + off + i * 24)
                    self.rels.append((r_off, r_info & 0xffffffff, r_info >> 32, add))

    def tag(self, name):
        return self.tags[DT[name]][0]

    def u32(self, a):
        return struct.unpack_from('<I', self.img, OFF + a)[0]

    def u64(self, a):
        return struct.unpack_from('<Q', self.img, OFF + a)[0]

    def bytes(self, a, n):
        return self.img[OFF + a:OFF + a + n]

    def imports(self):
        return [s for s in self.syms[1:] if s[4] == 0 and s[0]]

    def exports(self):
        return [s for s in self.syms[1:] if s[4] != 0 and s[0]]

    def in_text(self, a):
        return self.text[0] <= a < self.text[1]

    def init_array(self):
        if not self.tags.get(DT['INIT_ARRAY']):
            return []
        a, n = self.tag('INIT_ARRAY'), self.tag('INIT_ARRAYSZ') // 8
        # the array is filled by RELATIVE relocations; read the addends
        rel = {r[0]: r[3] for r in self.rels if r[1] == 1027}
        return [rel.get(a + 8 * i, self.u64(a + 8 * i)) for i in range(n)]

    def code_seeds(self):
        seeds = {self.text[0]}
        for name, value, size, info, shndx in self.exports():
            if info & 0xf == 2 and self.in_text(value):
                seeds.add(value)
        for off, typ, sym, add in self.rels:
            if typ == 1027 and self.in_text(add) and add % 4 == 0:
                seeds.add(add)
        seeds.update(a for a in self.init_array() if self.in_text(a))
        return sorted(seeds)


def main():
    cmd, elf = sys.argv[1], sys.argv[2]
    m = Module(elf)
    if cmd == 'seeds':
        s = m.code_seeds()
        json.dump(s, open(sys.argv[3], 'w'))
        print('%s: %d seeds' % (elf, len(s)))
    elif cmd == 'info':
        print('text %x-%x ro %x-%x data %x-%x' % (*m.text, *m.ro, *m.data))
        print('syms %d imports %d exports %d' % (len(m.syms), len(m.imports()), len(m.exports())))
        print('relocs', dict(collections.Counter(R_AARCH64.get(t, t) for _, t, _, _ in m.rels)))
        print('init_array', [hex(a) for a in m.init_array()][:8], len(m.init_array()))


if __name__ == '__main__':
    main()
