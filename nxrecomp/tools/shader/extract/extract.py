"""Every shader of a Trinity-engine game (Pokemon Scarlet / Violet) from its files, for translation at build time.

Writes <build>/shaders_all/<hash>.bin in the runtime's dump format (nvn.c shader_dump: "MKSH", control size, code
size, stage; GLSLC control section; code), the hash being nvn.c's (FNV-1a 64 of the code up to the control
section's end), so the runtime finds each translation as if it had dumped the shader itself.

Sources (see pokemon-violet/CLAUDE.md, "Shaders in the game files"):
  - <dump>/romfs/arc/data.trpfs: "ONEPACK\\0", u64 offset of a FlatBuffers table {pack hashes, pack offsets};
    each pack a FlatBuffer {file hashes, files {.., compression, decompressed size, data}}, Oodle Kraken
    decoded by oozbatch.exe (build.bat: ooz, GPLv3, run as a separate process);
  - the modules (<dump>/elf/*.elf): the engine's own shaders in embedded BNSH files.
Shaders sit in BNSH (nn::gfx shader files), alone or inside VFXB / SARC files: u64 pairs {control offset, code
offset} relative to the BNSH.

Usage (inside the game's repo): python extract.py [--ooz <oozbatch.exe>]   (default <build>/ooz/oozbatch.exe)
Then: translate.py --spv <build>/shaders_all
"""
import os
import struct
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..'))
import gamecfg                                  # noqa: E402

CONTROL_MAGIC, CODE_MAGIC = b'\x34\x12\x76\x98', b'\x78\x56\x34\x12'


# ----------------------------------------------------------------------------- FlatBuffers, trpfs
def _table(b, pos):
    vt = pos - struct.unpack_from('<i', b, pos)[0]
    vlen = struct.unpack_from('<H', b, vt)[0]
    return [struct.unpack_from('<H', b, vt + 4 + 2 * i)[0] for i in range((vlen - 4) // 2)]


def _vec(b, pos):
    p = pos + struct.unpack_from('<I', b, pos)[0]
    return p + 4, struct.unpack_from('<I', b, p)[0]


def _ref(b, pos):
    return pos + struct.unpack_from('<I', b, pos)[0]


def name_hash(name):
    """a pack's hash from its name as data.trpfd stores it (FNV-1a 64 with the game's offset basis)"""
    h = 0xcbf29ce484222645
    for c in name:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


class Trpfs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(16)
        if h[:8] != b'ONEPACK\0':
            raise SystemExit('%s: not a trpfs' % path)
        off = struct.unpack_from('<Q', h, 8)[0]
        self.f.seek(off)
        b = self.f.read()
        r = _ref(b, 0)
        fl = _table(b, r)
        hp, n = _vec(b, r + fl[0])
        op, _ = _vec(b, r + fl[1])
        self.hashes = [struct.unpack_from('<Q', b, hp + 8 * i)[0] for i in range(n)]
        offs = [struct.unpack_from('<Q', b, op + 8 * i)[0] for i in range(n)]
        self.order = sorted(range(n), key=lambda i: offs[i])
        self.span = {}
        for k, i in enumerate(self.order):
            self.span[i] = (offs[i], offs[self.order[k + 1]] if k + 1 < n else off)

    def files(self, i):
        """the files of pack i: (hash, compressed, decompressed size, data)"""
        lo, hi = self.span[i]
        self.f.seek(lo)
        pk = self.f.read(hi - lo)
        r = _ref(pk, 0)
        fl = _table(pk, r)
        hp, n = _vec(pk, r + fl[0])
        fp, _ = _vec(pk, r + fl[1])
        for k in range(n):
            tp = _ref(pk, fp + 4 * k)
            ff = _table(pk, tp) + [0] * 6
            comp = pk[tp + ff[1]] if ff[1] else 0       # 3 with Oodle (field 2 then 4); absent: stored
            size = struct.unpack_from('<Q', pk, tp + ff[3])[0] if ff[3] else 0
            dp, dn = _vec(pk, tp + ff[4]) if ff[4] else (0, 0)
            yield struct.unpack_from('<Q', pk, hp + 8 * k)[0], comp != 0, size, pk[dp:dp + dn]


class Ooz:
    """oozbatch.exe: [u64 size][u32 length][data] in, [u32 length (0: failed)][data] out"""
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def __call__(self, size, data):
        self.p.stdin.write(struct.pack('<QI', size, len(data)) + data)
        self.p.stdin.flush()
        n = struct.unpack('<I', self.p.stdout.read(4))[0]
        return self.p.stdout.read(n) if n else None


# ----------------------------------------------------------------------------- shaders
def fnv64(d):
    """nvn.c's shader hash (FNV-1a 64, standard offset basis)"""
    h = 0xcbf29ce484222325
    for c in d:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def code_end(b, c):
    """the code's length as nvn.c shader_intern takes it from the control section"""
    end = struct.unpack_from('<I', b, c + 0x704)[0]
    if end < 0x80 or end > 0x100000:
        end = 0x30 + struct.unpack_from('<I', b, c + 0x6f8)[0]
    return end


def stage(code):
    """the runtime's stage numbers (nvn.c shader_stage): 0 vertex, 1 fragment, 2 tess control, 3 tess eval,
    4 geometry, 5 compute"""
    if code[:4] != CODE_MAGIC:
        return 5
    w0 = struct.unpack_from('<I', code, 0x30)[0]
    if w0 & 0x1f == 2:
        return 1
    if w0 & 0x1f == 1:
        return {1: 0, 2: 2, 3: 3, 4: 4}.get(w0 >> 10 & 0xf, 5)
    return 5


def bnsh_shaders(s):
    """(control offset, code offset, length) of each shader in one BNSH file"""
    ctl = set()
    p = s.find(CONTROL_MAGIC)
    while p >= 0:
        ctl.add(p)
        p = s.find(CONTROL_MAGIC, p + 4)
    if not ctl:
        return
    a = np.frombuffer(s[:len(s) // 8 * 8], dtype='<u8')
    seen = set()
    for i in np.nonzero(np.isin(a, np.array(sorted(ctl), dtype=np.uint64)))[0].tolist():
        if i + 1 >= len(a):
            continue
        c, code = int(a[i]), int(a[i + 1])
        if (c, code) in seen or code + 0x30 > len(s) or c + 0x7d0 > len(s):
            continue
        seen.add((c, code))
        end = code_end(s, c)
        if code + end <= len(s):
            yield c, code, end


class Writer:
    def __init__(self, out):
        self.out, self.hashes, self.bodies, self.bnsh = out, set(), set(), 0
        os.makedirs(out, exist_ok=True)

    def scan(self, b):
        """every BNSH in a file (alone or embedded)"""
        p = b.find(b'BNSH\0\0\0\0')
        while p >= 0:
            size = struct.unpack_from('<I', b, p + 0x1c)[0]
            s = b[p:p + size] if size > 0x40 else b[p:]
            self.bnsh += 1
            for c, code, end in bnsh_shaders(s):
                body = s[code:code + end]
                key = (len(body), hash(body))
                if key in self.bodies:
                    continue
                self.bodies.add(key)
                h = fnv64(body)
                if h in self.hashes:
                    continue
                self.hashes.add(h)
                with open(os.path.join(self.out, '%016x.bin' % h), 'wb') as f:
                    f.write(struct.pack('<4I', 0x48534b4d, 0x7d0, len(body), stage(body)))
                    f.write(s[c:c + 0x7d0])
                    f.write(body)
            p = b.find(b'BNSH\0\0\0\0', p + 8)


def main():
    ooz = os.path.join(gamecfg.BUILD, 'ooz', 'oozbatch.exe')
    if '--ooz' in sys.argv:
        ooz = sys.argv[sys.argv.index('--ooz') + 1]
    if not os.path.exists(ooz):
        raise SystemExit('no %s: build it with build.bat' % ooz)
    w = Writer(os.path.join(gamecfg.BUILD, 'shaders_all'))
    t0 = time.time()
    for m in gamecfg.MODULES:
        w.scan(open(os.path.join(gamecfg.ELF, m + '.elf'), 'rb').read())
    print('modules: %d shaders' % len(w.hashes), flush=True)
    t, dec = Trpfs(os.path.join(gamecfg.DUMP, 'romfs', 'arc', 'data.trpfs')), Ooz(ooz)
    nfiles = failed = 0
    for k, i in enumerate(t.order):
        for _, packed, size, data in t.files(i):
            b = dec(size, data) if packed and size else data if not packed else b''
            nfiles += 1
            if b is None:
                failed += 1
                continue
            w.scan(b)
        if k % 2000 == 0:
            print('pack %d/%d: %d shaders' % (k, len(t.order), len(w.hashes)), flush=True)
    print('%d files (%d not decoded), %d BNSH, %d shaders in %s (%.0f s)' %
          (nfiles, failed, w.bnsh, len(w.hashes), w.out, time.time() - t0))


if __name__ == '__main__':
    main()
