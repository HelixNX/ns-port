"""Opcode histogram of the dumped shaders: python stats.py [shader dir]"""
import collections, glob, os, sys
from maxwell import Shader, opcode

d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '../../../build/shaders')
ops, st, unknown, where = collections.Counter(), collections.Counter(), collections.Counter(), {}
total = 0
for f in sorted(glob.glob(os.path.join(d, '*.bin'))):
    s = Shader(f)
    st[s.stage] += 1
    for pc, w in s.instructions():
        o = opcode(w)
        ops[o] += 1
        total += 1
        if o == '?':
            unknown[w >> 48] += 1
            where.setdefault(w >> 48, (os.path.basename(f), hex(pc), hex(w)))
print(sum(st.values()), 'shaders', dict(st), total, 'instructions')
print(' '.join('%s:%d' % kv for kv in ops.most_common()))
print('unknown', [(hex(k), v, where[k]) for k, v in unknown.most_common(10)])
