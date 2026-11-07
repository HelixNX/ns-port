"""Print a FlatBuffers binary schema (.bfbs, reflection.fbs) as tables and enums: field name, slot id, base type.

    python tools/bfbs.py <file.bfbs> [<path in the romfs>]    (a romfs path is read through nxrecomp/tools/trinity.py)
"""
import os
import struct
import sys

TYPES = ['None', 'UType', 'Bool', 'Byte', 'UByte', 'Short', 'UShort', 'Int', 'UInt', 'Long', 'ULong', 'Float',
         'Double', 'String', 'Vector', 'Obj', 'Union', 'Array', 'Vector64']


class T:
    def __init__(self, b, pos):
        self.b, self.pos = b, pos
        vt = pos - struct.unpack_from('<i', b, pos)[0]
        n = struct.unpack_from('<H', b, vt)[0]
        self.slots = [struct.unpack_from('<H', b, vt + 4 + 2 * i)[0] for i in range((n - 4) // 2)]

    def off(self, i):
        return self.slots[i] if i < len(self.slots) else 0

    def scalar(self, i, fmt, d=0):
        o = self.off(i)
        return struct.unpack_from(fmt, self.b, self.pos + o)[0] if o else d

    def ref(self, i):
        o = self.off(i)
        if not o:
            return None
        p = self.pos + o
        return p + struct.unpack_from('<I', self.b, p)[0]

    def str(self, i):
        p = self.ref(i)
        if p is None:
            return ''
        n = struct.unpack_from('<I', self.b, p)[0]
        return self.b[p + 4:p + 4 + n].decode('utf-8', 'replace')

    def vec(self, i):
        p = self.ref(i)
        if p is None:
            return []
        n = struct.unpack_from('<I', self.b, p)[0]
        return [T(self.b, p + 4 + 4 * k + struct.unpack_from('<I', self.b, p + 4 + 4 * k)[0]) for k in range(n)]

    def obj(self, i):
        p = self.ref(i)
        return T(self.b, p) if p is not None else None


def dump(b):
    root = T(b, struct.unpack_from('<I', b, 0)[0])
    objs = root.vec(0)
    enums = root.vec(1)
    names = [o.str(0) for o in objs]
    for e in enums:
        vals = ['%s=%d' % (v.str(0), v.scalar(1, '<q')) for v in e.vec(1)]
        print('enum %s: %s' % (e.str(0), ', '.join(vals)))
    for o in objs:
        print('%s %s (bytesize %d)' % ('struct' if o.scalar(2, '<B') else 'table', o.str(0), o.scalar(4, '<i')))
        for f in sorted(o.vec(1), key=lambda f: f.scalar(2, '<H')):
            ty = f.obj(1)
            bt, el, idx = ty.scalar(0, '<b'), ty.scalar(1, '<b'), ty.scalar(2, '<i', -1)
            tn = TYPES[bt]
            if bt == 14:
                tn = 'Vector<%s>' % (names[idx] if TYPES[el] == 'Obj' and idx >= 0 else TYPES[el])
            elif bt == 15:
                tn = names[idx] if idx >= 0 else 'Obj'
            elif idx >= 0 and bt in range(1, 11):
                tn += ' (enum %s)' % enums[idx].str(0)
            print('    %2d %-24s %s  offset %d' % (f.scalar(2, '<H'), f.str(0), tn, f.scalar(3, '<H')))


def main():
    p = sys.argv[1]
    if len(sys.argv) > 2:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'nxrecomp', 'tools'))
        from trinity import Trinity
        b = Trinity().read(sys.argv[2])
    else:
        b = open(p, 'rb').read()
    dump(b)


if __name__ == '__main__':
    main()
