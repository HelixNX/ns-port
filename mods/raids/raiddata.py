"""The raid mod's data (raids.cpp) from the game's own files: <build>/mods/raids.dat. Game-derived: it stays in build/.

Read through nxrecomp/tools/trinity.py (data.trpfd / data.trpfs, Oodle via oozbatch.exe):
  world/data/raid/{,su1_,su2_}raid_enemy_0N/..._array.bin   the raid tables: Paldea, Kitakami, Blueberry; N = stars
  avalon/data/personal_array.bin                            types, abilities, gender ratio, DevID -> national dex
  message/dat/English/common/{monsname,wazaname,tokusei,seikaku,typename}.dat    names
  world/data/encount/pokedata/pokedata{,_su1,_su2}/..._array.bin   wild encounters, copied as they are to
                                                            <build>/mods/encount_pokedata{,_su1,_su2}.bin
Schemas (FlatBuffers; field = vtable slot, a missing field is its default 0):
  raid table {entries [{info}]}, info: 0 rom (0 both, 1 Scarlet, 2 Violet), 3 difficulty, 4 rate (u8),
    9 boss: 0 DevID u16, 1 form i16, 2 sex, 4 level i32, 7-10 moves {0 move u16, 1 PP ups}, 11 gem (0 the
    species' types, 1 random, 2+ type + 2), 12 nature (0 random, else + 1), 13 ability (0 1/2, 1 1/2/H, 2-4 fixed
    1/2/H), 16 flawless IVs u8, 18 shiny (0 random, 1 never, 2 always)
  personal {entries}: 0 {u16 DevID, form, national, ?}, 5 type 1, 6 type 2, 7-9 abilities u16, 12 gender
    {u8 group, u8 ratio: % female}
  message .dat: u16 sections, u16 lines, u32 size, u32 key, u32 section offset; at the section: u32 size, then
    {u32 offset, u16 length, u16 flags} a line; each line's u16s XOR a key that starts at 0x7C89 + line * 0x2983
    and rotates left 3 bits a character.

raids.dat (little-endian): "NXRD", u32 version 1; five name lists (species by the game's internal number (DevID: the National Dex up to Gen 8, not after), moves, abilities,
natures, types), each u32 count then {u16 length, UTF-8}; u32 count of raid entries, 38 bytes each:
  u8 region (0 Paldea, 1 Kitakami, 2 Blueberry), stars, rom, rate; u16 national, form; u8 level, gem, ability
  kind, flawless; u8 shiny kind, nature (0 random), type 1, type 2; u8 gender group, gender ratio, 0, 0;
  u16 abilities[3]; u16 moves[4]; u16 DevID, 0
in each table's order (the rate roll walks them in order).

Usage (inside violetrecomp): python mods/raids/raiddata.py
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', '..', 'nxrecomp', 'tools'))
import gamecfg                                              # noqa: E402
from trinity import Trinity                                 # noqa: E402
from extract import _table, _vec, _ref                      # noqa: E402

REGIONS = [('', 'Paldea'), ('su1_', 'Kitakami'), ('su2_', 'Blueberry')]
# PKHeX's per-star rate totals (Scarlet, Violet), to check the tables against
PKHEX_TOTALS = {
    'Paldea': {1: (5800,) * 2, 2: (5300,) * 2, 3: (7400,) * 2, 4: (8800, 8700), 5: (9100,) * 2, 6: (6500,) * 2},
    'Kitakami': {1: (1500,) * 2, 2: (1500,) * 2, 3: (2500,) * 2, 4: (2100,) * 2, 5: (2250,) * 2, 6: (2475, 2574)},
    'Blueberry': {1: (1100,) * 2, 2: (1100,) * 2, 3: (2000,) * 2, 4: (1900,) * 2, 5: (2100,) * 2, 6: (2600,) * 2},
}


def fields(b, pos):
    return _table(b, pos) + [0] * 32


def val(b, pos, f, k, fmt):
    return struct.unpack_from(fmt, b, pos + f[k])[0] if f[k] else 0


def entries(b):
    r = _ref(b, 0)
    vp, n = _vec(b, r + fields(b, r)[0])
    return [_ref(b, vp + 4 * k) for k in range(n)]


def messages(t, name):
    d = t.read('message/dat/English/common/%s.dat' % name)
    _, nlines, _, _, soff = struct.unpack_from('<HHIII', d, 0)
    out = []
    for i in range(nlines):
        off, ln, _ = struct.unpack_from('<IHH', d, soff + 4 + 8 * i)
        key = (0x7C89 + i * 0x2983) & 0xffff
        cs = []
        for j in range(ln):
            cs.append(struct.unpack_from('<H', d, soff + off + 2 * j)[0] ^ key)
            key = ((key << 3) | (key >> 13)) & 0xffff
        s, j = '', 0
        while j < len(cs) and cs[j]:
            if cs[j] == 0x10:                   # a variable: 0x10, its length, its words
                j += 2 + (cs[j + 1] if j + 1 < len(cs) else 0)
                continue
            s += chr(cs[j])
            j += 1
        out.append(s)
    return out


def personal(t):
    b = t.read('avalon/data/personal_array.bin')
    out = {}
    for e in entries(b):
        f = fields(b, e)
        if not f[0]:
            continue
        dev, form, nat, _ = struct.unpack_from('<4H', b, e + f[0])
        g = b[e + f[12]:e + f[12] + 2] if f[12] else b'\0\0'
        out[(dev, form)] = dict(national=nat, t1=val(b, e, f, 5, '<B'), t2=val(b, e, f, 6, '<B'),
                                abil=[val(b, e, f, k, '<H') for k in (7, 8, 9)], ggroup=g[0], gratio=g[1])
    return out


def main():
    t = Trinity()
    pers = personal(t)
    names = [messages(t, n) for n in ('monsname', 'wazaname', 'tokusei', 'seikaku', 'typename')]
    rows = []
    for ri, (pre, region) in enumerate(REGIONS):
        for stars in range(1, 7):
            path = 'world/data/raid/{p}raid_enemy_{s:02d}/{p}raid_enemy_{s:02d}_array.bin'.format(p=pre, s=stars)
            if not t.has(path):
                print('%s: none' % path)
                continue
            b = t.read(path)
            tot = [0, 0]
            for e in entries(b):
                info = _ref(b, e + fields(b, e)[0])
                fi = fields(b, info)
                rom, rate = val(b, info, fi, 0, '<h'), val(b, info, fi, 4, '<B')
                poke = _ref(b, info + fi[9])
                fp = fields(b, poke)
                dev, form = val(b, poke, fp, 0, '<H'), val(b, poke, fp, 1, '<h')
                moves = []
                for k in (7, 8, 9, 10):
                    if fp[k]:
                        w = _ref(b, poke + fp[k])
                        moves.append(val(b, w, fields(b, w), 0, '<H'))
                    else:
                        moves.append(0)
                p = pers.get((dev, form)) or pers.get((dev, 0))
                if not p:
                    raise SystemExit('%s: DevID %d form %d not in personal_array' % (path, dev, form))
                for v in (0, 1):
                    if rom in (0, v + 1):
                        tot[v] += rate
                rows.append(struct.pack('<4B2H4B4B4B3H4H2H', ri, stars, rom, rate, p['national'], form,
                                        val(b, poke, fp, 4, '<i'), val(b, poke, fp, 11, '<i'),
                                        val(b, poke, fp, 13, '<i'), val(b, poke, fp, 16, '<B'),
                                        val(b, poke, fp, 18, '<i'), val(b, poke, fp, 12, '<i'), p['t1'], p['t2'],
                                        p['ggroup'], p['gratio'], 0, 0, *p['abil'], *moves, dev, 0))
            want = PKHEX_TOTALS[region][stars]
            print('%-9s %d*: %3d entries, rate totals Scarlet %d Violet %d%s' % (
                region, stars, len(entries(b)), tot[0], tot[1],
                '' if tuple(tot) == want else '  (PKHeX: %d / %d)' % want))
    out = os.path.join(gamecfg.BUILD, 'mods')
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, 'raids.dat')
    with open(path, 'wb') as f:
        f.write(b'NXRD' + struct.pack('<I', 1))
        for lst in names:
            f.write(struct.pack('<I', len(lst)))
            for s in lst:
                u = s.encode('utf-8')
                f.write(struct.pack('<H', len(u)) + u)
        f.write(struct.pack('<I', len(rows)))
        f.write(b''.join(rows))
    print('%s: %d raids, %d species names' % (path, len(rows), len(names[0])))
    # the wild encounter tables as the game has them, for F8's wild spawns (mons.cpp finds them in memory)
    for sub in ('', '_su1', '_su2'):
        src = 'world/data/encount/pokedata/pokedata%s/pokedata%s_array.bin' % (sub, sub)
        d = t.read(src)
        dst = os.path.join(out, 'encount_pokedata%s.bin' % sub)
        open(dst, 'wb').write(d)
        print('%s: %d bytes' % (dst, len(d)))


if __name__ == '__main__':
    main()
