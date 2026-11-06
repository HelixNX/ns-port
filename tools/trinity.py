"""Files of a Trinity-engine game (Pokemon Scarlet / Violet) by path, from its romfs's arc/data.trpfd + data.trpfs.

data.trpfd (FlatBuffers): {file hashes u64[], pack names string[], file infos {pack index u64}[], pack infos[]};
a file's hash is FNV-1a 64 with the game's offset basis of its path ("world/data/raid/.../x.bin"), a pack's of its
name. data.trpfs: see shader/extract/extract.py (Trpfs). Packed files are Oodle Kraken, decoded by oozbatch.exe
(shader/extract/build.bat; GPLv3, run as a separate process).

    t = Trinity()                      # <dump>/romfs/arc, <build>/ooz/oozbatch.exe (game.json)
    data = t.read('world/data/raid/raid_enemy_01/raid_enemy_01_array.bin')

Usage: python trinity.py <path in the romfs> <out file>
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'shader', 'extract'))
import gamecfg                                  # noqa: E402
from extract import Trpfs, Ooz, _table, _vec, _ref, name_hash   # noqa: E402


class Trinity:
    def __init__(self, arc=None, ooz=None):
        arc = arc or os.path.join(gamecfg.DUMP, 'romfs', 'arc')
        ooz = ooz or os.path.join(gamecfg.BUILD, 'ooz', 'oozbatch.exe')
        if not os.path.exists(ooz):
            raise SystemExit('no %s: build it with nxrecomp/tools/shader/extract/build.bat' % ooz)
        b = open(os.path.join(arc, 'data.trpfd'), 'rb').read()
        r = _ref(b, 0)
        fl = _table(b, r)
        hp, n = _vec(b, r + fl[0])
        fp, _ = _vec(b, r + fl[2])
        np_, nn = _vec(b, r + fl[1])
        self.pack_of = {}                       # file hash -> pack name hash
        names = []
        for k in range(nn):
            s = _ref(b, np_ + 4 * k)
            names.append(b[s + 4:s + 4 + struct.unpack_from('<I', b, s)[0]])
        for i in range(n):
            t = _ref(b, fp + 4 * i)
            f = _table(b, t)
            pk = struct.unpack_from('<Q', b, t + f[0])[0] if f and f[0] else 0
            self.pack_of[struct.unpack_from('<Q', b, hp + 8 * i)[0]] = name_hash(names[pk])
        self.fs = Trpfs(os.path.join(arc, 'data.trpfs'))
        self.pack_index = {h: i for i, h in enumerate(self.fs.hashes)}
        self.ooz = Ooz(ooz)
        self.cache = {}                         # pack index -> {file hash: (packed, size, data)}

    def has(self, path):
        return name_hash(path.encode()) in self.pack_of

    def read(self, path):
        h = name_hash(path.encode())
        if h not in self.pack_of:
            raise KeyError(path)
        i = self.pack_index[self.pack_of[h]]
        if i not in self.cache:
            self.cache[i] = {fh: (packed, size, data) for fh, packed, size, data in self.fs.files(i)}
        packed, size, data = self.cache[i][h]
        if not packed:
            return data
        out = self.ooz(size, data) if size else b''
        if out is None:
            raise IOError('%s: Oodle decoding failed' % path)
        return out


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    d = Trinity().read(sys.argv[1])
    open(sys.argv[2], 'wb').write(d)
    print('%s: %d bytes' % (sys.argv[2], len(d)))
