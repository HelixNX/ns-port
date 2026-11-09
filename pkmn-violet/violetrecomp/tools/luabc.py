"""Read Violet's Lua 5.4 bytecode (romfs script/lua/bin/release/.../*.blua; build/lua/ via trinity.py): functions,
constants, a plain listing. Game-derived output stays in build/.

    python tools/luabc.py <file.blua> find <regex>        functions whose constants match (any string constant)
    python tools/luabc.py <file.blua> dis <path>          listing of the function at <path> (as `find` prints it)
    python tools/luabc.py <file.blua> callers <name>      functions that use the string constant <name> exactly

Format: the stock 5.4 dump (lundump.c): header, then a function: source, line defined, last line, params, vararg,
max stack, code, constants, upvalues, protos, debug info; sizes are MSB-first 7-bit varints (last byte | 0x80).
"""
import re
import struct
import sys

OPS = ('MOVE LOADI LOADF LOADK LOADKX LOADFALSE LFALSESKIP LOADTRUE LOADNIL GETUPVAL SETUPVAL GETTABUP GETTABLE GETI '
       'GETFIELD SETTABUP SETTABLE SETI SETFIELD NEWTABLE SELF ADDI ADDK SUBK MULK MODK POWK DIVK IDIVK BANDK BORK '
       'BXORK SHRI SHLI ADD SUB MUL MOD POW DIV IDIV BAND BOR BXOR SHL SHR MMBIN MMBINI MMBINK UNM BNOT NOT LEN '
       'CONCAT CLOSE TBC JMP EQ LT LE EQK EQI LTI LEI GTI GEI TEST TESTSET CALL TAILCALL RETURN RETURN0 RETURN1 '
       'FORLOOP FORPREP TFORPREP TFORCALL TFORLOOP SETLIST CLOSURE VARARG VARARGPREP EXTRAARG').split()


class Func:
    pass


class Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def byte(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def size(self):
        x = 0
        while True:
            c = self.byte()
            x = (x << 7) | (c & 0x7f)
            if c & 0x80:
                return x

    def string(self):
        n = self.size()
        if not n:
            return None
        s = self.b[self.p:self.p + n - 1]
        self.p += n - 1
        return s.decode('utf-8', 'replace')

    def func(self, path, src):
        f = Func()
        f.path = path
        f.source = self.string() or src
        f.line, f.lastline = self.size(), self.size()
        f.params, f.vararg, f.stack = self.byte(), self.byte(), self.byte()
        n = self.size()
        f.code = struct.unpack_from('<%dI' % n, self.b, self.p)
        self.p += 4 * n
        f.k = []
        for _ in range(self.size()):
            t = self.byte()
            if t == 0x00:
                f.k.append(None)
            elif t == 0x01:
                f.k.append(False)
            elif t == 0x11:
                f.k.append(True)
            elif t == 0x03:
                f.k.append(struct.unpack_from('<q', self.b, self.p)[0]); self.p += 8
            elif t == 0x13:
                f.k.append(struct.unpack_from('<d', self.b, self.p)[0]); self.p += 8
            elif t in (0x04, 0x14):
                f.k.append(self.string())
            else:
                raise ValueError('constant type %#x at %#x' % (t, self.p))
        f.upvals = [self.b[self.p + 3 * i:self.p + 3 * i + 3] for i in range(self.size())]
        self.p += 3 * len(f.upvals)
        f.protos = [self.func('%s/%d' % (path, i), f.source) for i in range(self.size())]
        n = self.size(); f.lineinfo = self.b[self.p:self.p + n]; self.p += n
        for _ in range(self.size()):
            self.size(); self.size()
        f.locals = []
        for _ in range(self.size()):
            f.locals.append((self.string(), self.size(), self.size()))
        f.upnames = [self.string() for _ in range(self.size())]
        return f


def load(path):
    b = open(path, 'rb').read()
    if b[:5] != b'\x1bLua\x54':
        raise SystemExit('%s: not Lua 5.4 bytecode' % path)
    r = Reader(b)
    r.p = 4 + 1 + 1 + 6 + 3 + 8 + 8 + 1
    return r.func('0', '?')


def walk(f):
    yield f
    for p in f.protos:
        yield from walk(p)


def strs(f):
    return [k for k in f.k if isinstance(k, str)]


def name_of(f):
    """the function's name where its parent stores it (SETFIELD / SETTABUP with a constant key right after CLOSURE)"""
    return getattr(f, 'name', '')


def assign_names(root):
    """name a function after where its parent stores it: Owner.name / Owner.prototype.name (CLOSURE, then SETFIELD /
    SETTABUP of that register under a constant key; the owner from the instruction that last loaded the table)"""
    for f in walk(root):
        src = {}                                    # register -> what was last loaded into it
        kv = {}                                     # register -> constant LOADKed into it
        for pc, ins in enumerate(f.code):
            op = OPS[ins & 0x7f] if (ins & 0x7f) < len(OPS) else '?'
            a, b, c, k = (ins >> 7) & 0xff, (ins >> 16) & 0xff, (ins >> 24) & 0xff, (ins >> 15) & 1
            if op == 'LOADK' and (ins >> 15) < len(f.k):
                kv[a] = f.k[ins >> 15]
                src.pop(a, None)
                continue
            if op == 'GETUPVAL':
                src[a] = ''
                kv.pop(a, None)
                continue
            if op == 'GETTABLE' and isinstance(kv.get(c), str):
                base = src.get(b, '?')
                src[a] = (base + '.' if base else '') + kv[c]
                kv.pop(a, None)
                continue
            if op == 'SETTABLE' and isinstance(kv.get(b), str):
                for back in range(pc - 1, max(-1, pc - 4), -1):
                    pi = f.code[back]
                    if OPS[pi & 0x7f] == 'CLOSURE' and ((pi >> 7) & 0xff) == c:
                        bx = pi >> 15
                        if bx < len(f.protos):
                            owner = src.get(a, '?')
                            f.protos[bx].name = (owner + '.' if owner else '') + kv[b]
                        break
                continue
            kv.pop(a, None)
            if op == 'GETTABUP' and c < len(f.k):
                src[a] = str(f.k[c])
            elif op == 'GETFIELD' and c < len(f.k):
                src[a] = '%s.%s' % (src.get(b, '?'), f.k[c])
            elif op == 'MOVE':
                src[a] = src.get(b, '?')
            elif op in ('SETFIELD', 'SETTABUP') and not k and b < len(f.k) and isinstance(f.k[b], str):
                for back in range(pc - 1, max(-1, pc - 4), -1):
                    pi = f.code[back]
                    if OPS[pi & 0x7f] == 'CLOSURE' and ((pi >> 7) & 0xff) == c:
                        bx = pi >> 15
                        if bx < len(f.protos):
                            owner = src.get(a, '?') if op == 'SETFIELD' else ''
                            f.protos[bx].name = (owner + '.' if owner else '') + f.k[b]
                        break
                    if ((pi >> 7) & 0xff) == c:
                        break
            elif op not in ('SETFIELD', 'SETTABUP', 'SETI', 'SETTABLE', 'JMP', 'EQ', 'LT', 'LE', 'EQK', 'EQI',
                            'TEST', 'RETURN', 'RETURN0', 'RETURN1', 'SETLIST'):
                src.pop(a, None)


def rk(f, i, isk):
    if isk:
        return repr(f.k[i]) if i < len(f.k) else 'K?%d' % i
    return 'r%d' % i


def dis(f):
    out = ['function %s %s (%s:%d-%d, %d params) locals %s' % (f.path, name_of(f), f.source, f.line, f.lastline,
                                                                f.params, [l[0] for l in f.locals])]
    for pc, ins in enumerate(f.code):
        op = OPS[ins & 0x7f] if (ins & 0x7f) < len(OPS) else '?%d' % (ins & 0x7f)
        a = (ins >> 7) & 0xff
        kf = (ins >> 15) & 1
        b = (ins >> 16) & 0xff
        c = (ins >> 24) & 0xff
        bx = ins >> 15
        sbx = bx - 0xffff
        if op in ('GETFIELD',):
            t = '%s r%d = r%d[%s]' % (op, a, b, rk(f, c, True))
        elif op == 'GETTABUP':
            t = '%s r%d = U%d[%s]' % (op, a, b, rk(f, c, True))
        elif op == 'SETTABUP':
            t = '%s U%d[%s] = %s' % (op, a, rk(f, b, True), rk(f, c, kf))
        elif op == 'SETFIELD':
            t = '%s r%d[%s] = %s' % (op, a, rk(f, b, True), rk(f, c, kf))
        elif op == 'SELF':
            t = '%s r%d = r%d:%s' % (op, a, b, rk(f, c, kf))
        elif op in ('LOADK',):
            t = '%s r%d = %s' % (op, a, rk(f, bx, True))
        elif op in ('LOADI', 'LOADF'):
            t = '%s r%d = %d' % (op, a, sbx)
        elif op in ('EQK',):
            t = '%s r%d == %s k=%d' % (op, a, rk(f, b, True), (ins >> 15) & 1)
        elif op in ('CALL', 'TAILCALL'):
            t = '%s r%d(%s args) -> %s' % (op, a, b - 1 if b else 'var', c - 1 if c else 'var')
        elif op in ('JMP',):
            t = '%s %+d' % (op, ((ins >> 7) & 0x1ffffff) - 0xffffff)
        elif op == 'CLOSURE':
            t = '%s r%d = %s/%d %s' % (op, a, f.path, bx, name_of(f.protos[bx]) if bx < len(f.protos) else '')
        elif op in ('GETUPVAL', 'SETUPVAL'):
            t = '%s r%d U%d(%s)' % (op, a, b, f.upnames[b] if b < len(f.upnames) else '')
        else:
            t = '%s a=%d b=%d c=%d k=%d' % (op, a, b, c, kf)
        out.append('  %4d  %s' % (pc, t))
    return '\n'.join(out)


def main():
    root = load(sys.argv[1])
    assign_names(root)
    cmd, arg = sys.argv[2], sys.argv[3]
    if cmd == 'find':
        rx = re.compile(arg)
        for f in walk(root):
            hits = [s for s in strs(f) if rx.search(s)]
            if hits:
                print('%s %s (%d-%d, %d ins): %s' % (f.path, name_of(f), f.line, f.lastline, len(f.code), hits[:8]))
    elif cmd == 'callers':
        for f in walk(root):
            if arg in strs(f):
                print('%s %s (%d-%d, %d ins)' % (f.path, name_of(f), f.line, f.lastline, len(f.code)))
    elif cmd == 'dis':
        for f in walk(root):
            if f.path == arg:
                print(dis(f))
                for p in f.protos:
                    print('   proto %s %s' % (p.path, name_of(p)))


if __name__ == '__main__':
    main()
