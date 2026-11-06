"""Function list of a module from its own unwind tables: no IDA / Ghidra needed (the player's build).

Every function the compiler gave unwind information has an FDE in .eh_frame, indexed by .eh_frame_hdr (found by
its header in the read-only segment): its start and length become a function. Code without an FDE (PLT stubs,
hand-written assembly) and jump tables are left to the lifter's own recovery (a64lift.py recover_functions /
recover_switches), which starts from these functions and nxmod.py's code seeds. Writes the same
<module>_ida.json as ida_export.py / ghidra_export.ps1 (no text_data, no switches).

Usage: python funcs.py <module.elf> <out.json>
       python funcs.py compare <module.elf> <reference_ida.json>     how the list compares with IDA's / Ghidra's
"""
import json
import struct
import sys

from nxmod import Module, OFF


def _uleb(b, i):
    v = s = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7f) << s
        s += 7
        if not x & 0x80:
            return v, i


def _sleb(b, i):
    v = s = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7f) << s
        s += 7
        if not x & 0x80:
            if x & 0x40:
                v -= 1 << s
            return v, i


def _enc(img, i, enc, pc):
    """Read a DW_EH_PE value at image index i (module address pc); returns (value, next index)."""
    fmt = enc & 0x0f
    if fmt == 0x00 or fmt == 0x0c:
        v, n = struct.unpack_from('<Q', img, i)[0], 8
        if fmt == 0x0c:
            v = struct.unpack_from('<q', img, i)[0]
    elif fmt == 0x03:
        v, n = struct.unpack_from('<I', img, i)[0], 4
    elif fmt == 0x0b:
        v, n = struct.unpack_from('<i', img, i)[0], 4
    elif fmt == 0x02:
        v, n = struct.unpack_from('<H', img, i)[0], 2
    elif fmt == 0x0a:
        v, n = struct.unpack_from('<h', img, i)[0], 2
    elif fmt == 0x01:
        return _uleb(img, i)
    elif fmt == 0x09:
        return _sleb(img, i)
    else:
        raise ValueError('pointer encoding %#x' % enc)
    if enc & 0x70 == 0x10:
        v += pc
    elif enc & 0x70 not in (0, ):
        raise ValueError('pointer application %#x' % enc)
    return v & 0xffffffffffffffff, i + n


def eh_frame_hdr(m):
    """Module address of .eh_frame_hdr (version 1, eh_frame_ptr pcrel sdata4, count udata4, table datarel sdata4)."""
    ro0, ro1 = m.ro
    sig = b'\x01\x1b\x03\x3b'
    i = m.img.find(sig, OFF + ro0, OFF + ro1)
    while i >= 0:
        a = i - OFF
        ptr = a + 4 + struct.unpack_from('<i', m.img, i + 4)[0]
        n = struct.unpack_from('<I', m.img, i + 8)[0]
        if ro0 <= ptr < ro1 and 0 < n and a + 12 + 8 * n <= ro1:
            return a, n
        i = m.img.find(sig, i + 1, OFF + ro1)
    return None, 0


def functions(m):
    """[(start, end)] of every FDE, module offsets, sorted."""
    hdr, n = eh_frame_hdr(m)
    if hdr is None:
        return []
    img, cies, out = m.img, {}, []
    for k in range(n):
        fde = hdr + struct.unpack_from('<i', img, OFF + hdr + 12 + 8 * k + 4)[0]
        i = OFF + fde
        length = struct.unpack_from('<I', img, i)[0]
        assert length != 0xffffffff, '64-bit DWARF'
        cie = fde + 4 - struct.unpack_from('<I', img, i + 4)[0]
        if cie not in cies:
            cies[cie] = _cie_encoding(img, OFF + cie)
        enc = cies[cie]
        start, j = _enc(img, i + 8, enc, fde + 8)
        size, _ = _enc(img, j, enc & 0x0f, 0)
        if size and m.in_text(start):
            out.append((start, start + size))
    return sorted(set(out))


def _cie_encoding(img, i):
    """The FDE pointer encoding ('R' augmentation) of the CIE at image index i."""
    j = i + 9                                     # length, CIE id, version
    end = img.index(b'\0', j)
    aug = img[j:end].decode('ascii')
    j = end + 1
    _, j = _uleb(img, j)                          # code alignment
    _, j = _sleb(img, j)                          # data alignment
    _, j = _uleb(img, j)                          # return address register (version 3; 1 byte in version 1)
    if not aug.startswith('z'):
        return 0x00
    _, j = _uleb(img, j)                          # augmentation data length
    for c in aug[1:]:
        if c == 'R':
            return img[j]
        if c == 'P':
            pe = img[j]
            _, j = _enc(img, j + 1, pe & 0x0f, 0)
        elif c in 'LB':
            j += 1
        elif c == 'S':
            pass
        else:
            break
    return 0x00


def export(path, out):
    m = Module(path)
    fs = functions(m)
    js = {'module': path, 'text': list(m.text), 'source': 'eh_frame',
          'functions': [{'ea': s, 'end': e, 'chunks': [[s, e]], 'noret': False, 'thunk': False} for s, e in fs],
          'text_data': [], 'switches': {}}
    with open(out, 'w') as f:
        json.dump(js, f)
    print('%s: %d functions from .eh_frame' % (out, len(fs)))


def compare(path, ref):
    m = Module(path)
    ours = dict(functions(m))
    theirs = {f['ea']: f for f in json.load(open(ref))['functions']}
    both = set(ours) & set(theirs)
    t0, t1 = m.text
    cov_ours = sum(e - s for s, e in ours.items())
    cov_theirs = sum(e - s for f in theirs.values() for s, e in f['chunks'])
    print('text %d KB; ours %d functions (%d KB), reference %d (%d KB), %d starts in both' % (
        (t1 - t0) >> 10, len(ours), cov_ours >> 10, len(theirs), cov_theirs >> 10, len(both)))
    only = sorted(set(theirs) - set(ours))
    small = sum(1 for a in only if theirs[a]['end'] - a <= 16)
    print('reference only: %d (%d of them <= 4 instructions, e.g. PLT stubs): %s' % (
        len(only), small, ' '.join('%x' % a for a in only[:12])))
    mine = sorted(set(ours) - set(theirs))
    print('ours only: %d: %s' % (len(mine), ' '.join('%x' % a for a in mine[:12])))
    differ = [a for a in both if ours[a] != theirs[a]['end']]
    print('same start, other end: %d: %s' % (len(differ), ' '.join(
        '%x(%x/%x)' % (a, ours[a], theirs[a]['end']) for a in differ[:8])))


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'compare':
        compare(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3:
        export(sys.argv[1], sys.argv[2])
    else:
        sys.exit(__doc__)
