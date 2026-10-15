"""Inventory of the 32-bit modules converted by nso2elf.py: symbols (imported / exported), relocation types,
ARM vs Thumb entry points, and which module provides each import of main.

Usage: python inventory.py <elf dir> [--imports <out.txt>]
"""
import collections
import os
import struct
import subprocess
import sys

MODULES = ['main', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3', 'subsdk4', 'sdk']
REL_NAMES = {2: 'ABS32', 21: 'GLOB_DAT', 22: 'JUMP_SLOT', 23: 'RELATIVE'}
OFF = 0x1000


class Elf32:
    def __init__(self, path):
        self.img = open(path, 'rb').read()
        assert self.img[4] == 1, '64-bit ELF; this tool reads the 32-bit modules'
        mem = self.img
        mod = struct.unpack_from('<I', mem, OFF + 4)[0]
        dyn = mod + struct.unpack_from('<i', mem, OFF + mod + 4)[0]
        self.tags = collections.defaultdict(list)
        n = 0
        while True:
            t, v = struct.unpack_from('<iI', mem, OFF + dyn + n * 8)
            n += 1
            if t == 0:
                break
            self.tags[t].append(v)
        symtab, strtab = self.tags[6][0], self.tags[5][0]
        nsym = struct.unpack_from('<I', mem, OFF + self.tags[4][0] + 4)[0] if 4 in self.tags else (strtab - symtab) // 16
        self.syms = []
        for i in range(nsym):
            name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', mem, OFF + symtab + i * 16)
            end = mem.index(b'\0', OFF + strtab + name)
            self.syms.append((mem[OFF + strtab + name:end].decode('ascii', 'replace'), value, size, info, shndx))
        self.rels = []
        for tag_off, tag_sz in ((17, 18), (23, 2)):        # DT_REL/DT_RELSZ, DT_JMPREL/DT_PLTRELSZ
            if tag_off in self.tags:
                off, sz = self.tags[tag_off][0], self.tags[tag_sz][0]
                for i in range(sz // 8):
                    r_off, r_info = struct.unpack_from('<II', mem, OFF + off + i * 8)
                    self.rels.append((r_off, r_info & 0xff, r_info >> 8))
        if 7 in self.tags:
            print('  note: module has DT_RELA (unexpected for AArch32)')

    def imports(self):
        return [s for s in self.syms[1:] if s[4] == 0 and s[0]]

    def exports(self):
        return [s for s in self.syms[1:] if s[4] != 0 and s[0]]


def demangle(names):
    if not names:
        return {}
    try:
        p = subprocess.run(['llvm-cxxfilt'], input='\n'.join(names), capture_output=True, text=True)
        return dict(zip(names, p.stdout.splitlines()))
    except OSError:
        return {n: n for n in names}


def main(elfdir, imports_out=None):
    # later versions ship fewer modules (3.0.1 has only main, sdk, subsdk0)
    mods = {m: Elf32(os.path.join(elfdir, m + '.elf')) for m in MODULES if os.path.exists(os.path.join(elfdir, m + '.elf'))}
    provider = {}
    for m in list(mods)[1:]:
        for name, value, size, info, shndx in mods[m].exports():
            provider.setdefault(name, m)
    for m, e in mods.items():
        imps, exps = e.imports(), e.exports()
        funcs = [s for s in exps if s[3] & 0xf == 2]
        thumb = sum(1 for s in funcs if s[1] & 1)
        relc = collections.Counter(REL_NAMES.get(t, str(t)) for _, t, _ in e.rels)
        print('%-8s imports %5d  exports %5d (func %5d, thumb %d)  relocs %s' % (
            m, len(imps), len(exps), len(funcs), thumb, dict(relc)))
        # what the module is: the most common namespaces among its exports
        ns = collections.Counter()
        for name, *_ in exps:
            if name.startswith('_ZN'):
                i, j = 3, 3
                while j < len(name) and name[j].isdigit():
                    j += 1
                if j > i:
                    k = int(name[i:j])
                    first = name[j:j + k]
                    if first == 'nn' and name[j + k:j + k + 1].isdigit():
                        j2 = j + k
                        i2 = j2
                        while j2 < len(name) and name[j2].isdigit():
                            j2 += 1
                        first = 'nn::' + name[j2:j2 + int(name[i2:j2])]
                    ns[first] += 1
            else:
                ns[name.split('_')[0] or name[:12]] += 1
        print('          top export namespaces:', ', '.join('%s(%d)' % kv for kv in ns.most_common(8)))
    # main's imports by providing module and by namespace
    imps = [s[0] for s in mods['main'].imports()]
    byprov = collections.Counter(provider.get(n, '?unresolved') for n in imps)
    print('\nmain imports by provider:', dict(byprov))
    dm = demangle(imps)
    if imports_out:
        with open(imports_out, 'w') as f:
            for n in sorted(imps, key=lambda n: (provider.get(n, '~'), dm[n])):
                f.write('%-8s %s\n' % (provider.get(n, '?'), dm[n]))
        print('wrote', imports_out)
    groups = collections.Counter()
    for n in imps:
        d = dm[n]
        key = '::'.join(d.split('(')[0].split('::')[:2]) if '::' in d.split('(')[0] else d.split('(')[0][:20]
        groups[(provider.get(n, '?'), key)] += 1
    print('\nmain import groups (provider, namespace):')
    for (p, k), c in groups.most_common(60):
        print('  %-8s %-40s %d' % (p, k, c))


if __name__ == '__main__':
    out = sys.argv[sys.argv.index('--imports') + 1] if '--imports' in sys.argv else None
    main(sys.argv[1], out)
