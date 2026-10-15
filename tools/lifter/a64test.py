"""Differential test of a64lift against Unicorn.

Instructions are sampled from the three modules, grouped by shape (capstone mnemonic and operand pattern),
up to --per instances of each shape. Every test runs one instruction from a random state (registers that are
pointers into a scratch window, small integers or random 64-bit values; V registers with a mix of floats and
random bits) in Unicorn and in the lifted C; registers, flags, the next PC and the scratch window are compared.

With --pairs, the samples are pairs of consecutive instructions: one that sets the flags, then one that reads
them (this checks the compare-and-branch folding, which spans two instructions).

Usage: python a64test.py [--per N] [--seed S] [--filter REGEX] [--states K] [--pairs]
Work files go to ../../../build/a64test.
"""
import collections
import os
import random
import re
import struct
import subprocess
import sys

import capstone
import unicorn
from unicorn import arm64_const as A

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import a64lift                                   # noqa: E402
from a64lift import Fn, Env, Program, emit      # noqa: E402
from a64insn import Unimpl, lift_insn           # noqa: E402

ROOT = a64lift.ROOT
WORK = os.path.join(ROOT, 'build', 'a64test')
WIN, WIN_SIZE = 0x7200000000, 0x20000
VCVARS = r'E:\Microsoft Visual Studio\VC\Auxiliary\Build\vcvars64.bat'
CLANG = r'C:\msys64\mingw64\bin\clang-cl.exe'
CFLAGS = ['/clang:-O1', '/clang:-ffp-contract=off', '/clang:-fno-strict-aliasing', '/clang:-march=haswell',
          '/clang:-w', '/clang:-fbracket-depth=1024']

SKIP = re.compile(r'^(svc|brk|udf|hlt|yield|wfe|wfi|sev|sevl|dc|ic|prfm|prfum|hint|mrs|msr|.byte|ldxr|ldaxr|ldxp|ldaxp'
                  r'|ldxrb|ldxrh|ldaxrb|ldaxrh|clrex)$')


class TestFn(Fn):
    def ret(self, expr):
        return '{ SAVE; c->pc = %s; c->branched = 1; return; }' % expr


class TestEnv(Env):
    def __init__(self, prog, mod):
        super().__init__(prog, mod)
        self.targets = set()

    def callee(self, target):
        self.targets.add(target)
        return 'tgt_%x' % target


def lift_test(env, words, pc, name):
    fn = TestFn(env, env.mod, pc, [])
    fn.order = [pc + 4 * i for i in range(len(words))]
    fn.addrs = set(fn.order[1:])
    lines = [(pc + 4 * i, lift_insn(fn, w, pc + 4 * i)) for i, w in enumerate(words)]
    fn.labels |= fn.addrs
    lines.append((None, '{ SAVE; if (!c->branched) c->pc = 0x%xull; return; }' % (pc + 4 * len(words))))
    src = emit(fn, lines)
    return src.replace('void f_%x(Ctx *c) {' % pc, 'static void %s(Ctx *c) {' % name, 1)


# ----------------------------------------------------------------------------- sampling
def shape(mn, ops):
    ops = re.sub(r'#-?0x[0-9a-f]+|#-?[0-9.e+-]+', '#', ops)
    ops = re.sub(r'\b([xwvqdshb])([0-9]+)\b', r'\1', ops)
    ops = re.sub(r'\[[0-9]+\]', '[i]', ops)
    ops = re.sub(r'0x[0-9a-f]+', 'ADDR', ops)
    return mn + ' ' + ops


SETS_FLAGS = re.compile(r'^(cmp|cmn|tst|adds|subs|ands|bics|negs|fcmp|fcmpe|ccmp|ccmn|fccmp|adcs|sbcs)$')
USES_FLAGS = re.compile(r'^(b\.\w+|csel|csinc|csinv|csneg|cset|csetm|cinc|cinv|cneg|ccmp|ccmn|fcsel|fccmp|adc|adcs|sbc|sbcs'
                        r'|ngc|ngcs)$')


def sample(prog, per, rng, filt, pairs=False):
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    groups = collections.defaultdict(list)
    for name, mod in prog.mods.items():
        code = mod.m.bytes(0, mod.text[1])
        insns = set()
        for off, offs in mod.funcs.items():
            if off in mod.plt:
                continue
            insns.update(offs)
        md.skipdata = True
        prev = None
        for a, sz, mn, ops in md.disasm_lite(code, mod.base):
            off = a - mod.base
            if sz != 4 or off not in insns or SKIP.match(mn):
                prev = None
                continue
            key = shape(mn, ops)
            w = struct.unpack_from('<I', code, off)[0]
            if pairs:                     # a flag-setting instruction followed by one that reads the flags
                cur = (a, mn, key, w)
                if prev is not None and prev[0] == a - 4 and SETS_FLAGS.match(prev[1]) and USES_FLAGS.match(mn):
                    pkey = prev[2] + ' ; ' + key
                    if not filt or re.search(filt, pkey):
                        groups[pkey].append((name, prev[0], (prev[3], w)))
                prev = cur
                continue
            if filt and not re.search(filt, key):
                continue
            groups[key].append((name, a, (w,)))
    tests = []
    for key in sorted(groups):
        items = groups[key]
        # distinct encodings first
        seen, uniq = set(), []
        for it in items:
            if it[2] not in seen:
                seen.add(it[2])
                uniq.append(it)
        rng.shuffle(uniq)
        for it in uniq[:per]:
            tests.append((key,) + it)
    return tests


# ----------------------------------------------------------------------------- states
F32_SPECIAL = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, float('inf'), float('-inf'), 1e-40, -1e-42, 3.4e38, 0.1]


def rand_f32(rng):
    r = rng.random()
    if r < 0.15:
        v = rng.choice(F32_SPECIAL)
    elif r < 0.35:
        v = float(rng.randint(-1000, 1000))
    elif r < 0.85:
        v = rng.uniform(-1000, 1000)
    elif r < 0.95:
        v = rng.uniform(-2, 2) * 2.0 ** rng.randint(-60, 60)
    else:
        return rng.getrandbits(32)
    return struct.unpack('<I', struct.pack('<f', v))[0]


def rand_f64(rng):
    r = rng.random()
    if r < 0.15:
        v = rng.choice(F32_SPECIAL + [1e-310, 1.7e308])
    elif r < 0.35:
        v = float(rng.randint(-100000, 100000))
    elif r < 0.85:
        v = rng.uniform(-1e6, 1e6)
    elif r < 0.95:
        v = rng.uniform(-2, 2) * 2.0 ** rng.randint(-200, 200)
    else:
        return rng.getrandbits(64)
    return struct.unpack('<Q', struct.pack('<d', v))[0]


def rand_state(rng):
    x = []
    for i in range(31):
        r = rng.random()
        if r < 0.4:
            x.append(WIN + 0x8000 + (rng.randrange(0x8000) & ~(7 if rng.random() < 0.7 else 0)))
        elif r < 0.6:
            x.append(rng.randrange(0x800))
        elif r < 0.7:
            x.append(rng.choice([0, 1, 0xffffffff, 0x80000000, 0x7fffffff, (1 << 64) - 1, 1 << 63, 0x100000000]))
        else:
            x.append(rng.getrandbits(64))
    sp = WIN + 0x8000 + (rng.randrange(0x8000) & ~15)
    v = []
    for i in range(32):
        k = rng.random()
        if k < 0.45:
            lanes = [rand_f32(rng) for _ in range(4)]
            v.append(lanes[0] | lanes[1] << 32 | lanes[2] << 64 | lanes[3] << 96)
        elif k < 0.75:
            v.append(rand_f64(rng) | rand_f64(rng) << 64)
        else:
            v.append(rng.getrandbits(128))
    nzcv = rng.randrange(16)
    return x, sp, v, nzcv


class Uni:
    def __init__(self, prog, mem_init):
        self.uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        try:
            self.uc.ctl_set_cpu_model(A.UC_CPU_ARM64_MAX)
        except Exception:
            pass
        for name, mod in prog.mods.items():
            m = mod.m
            size = (m.size + 0xfff) & ~0xfff
            self.uc.mem_map(mod.base, size, unicorn.UC_PROT_ALL)
            img = m.img[0x1000:0x1000 + min(len(m.img) - 0x1000, m.data[1])]
            self.uc.mem_write(mod.base, img[:size])
        self.uc.mem_map(WIN, WIN_SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
        self.mem_init = mem_init
        # enable FP/SIMD: CPACR_EL1.FPEN = 3
        self.uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 3 << 20)
        # data accesses must stay in the scratch window (the host test has nothing else mapped), except
        # PC-relative literal loads, which the lifter folds to constants
        self.literal = False
        self.outside = False

        def on_mem(uc, access, addr, size, value, user):
            if not (WIN <= addr and addr + size <= WIN + WIN_SIZE) and not self.literal:
                self.outside = True
                uc.emu_stop()
        self.uc.hook_add(unicorn.UC_HOOK_MEM_READ | unicorn.UC_HOOK_MEM_WRITE, on_mem)

    def run(self, pc, state, words=(0,)):
        uc = self.uc
        self.literal = any((w & 0x3b000000) == 0x18000000 for w in words)
        self.outside = False
        x, sp, v, nzcv = state
        uc.mem_write(WIN, self.mem_init)
        for i in range(29):
            uc.reg_write(A.UC_ARM64_REG_X0 + i, x[i])
        uc.reg_write(A.UC_ARM64_REG_X29, x[29])
        uc.reg_write(A.UC_ARM64_REG_X30, x[30])
        uc.reg_write(A.UC_ARM64_REG_SP, sp)
        for i in range(32):
            uc.reg_write(A.UC_ARM64_REG_Q0 + i, v[i])
        uc.reg_write(A.UC_ARM64_REG_NZCV, nzcv << 28)
        uc.reg_write(A.UC_ARM64_REG_FPCR, 0)
        uc.reg_write(A.UC_ARM64_REG_FPSR, 0)
        uc.emu_start(pc, pc + 0x100000, count=len(words))
        if self.outside:
            raise unicorn.UcError(unicorn.UC_ERR_READ_UNMAPPED)
        rx = [uc.reg_read(A.UC_ARM64_REG_X0 + i) for i in range(29)]
        rx += [uc.reg_read(A.UC_ARM64_REG_X29), uc.reg_read(A.UC_ARM64_REG_X30), uc.reg_read(A.UC_ARM64_REG_SP)]
        rv = [uc.reg_read(A.UC_ARM64_REG_Q0 + i) for i in range(32)]
        rn = uc.reg_read(A.UC_ARM64_REG_NZCV) >> 28
        rpc = uc.reg_read(A.UC_ARM64_REG_PC)
        mem = bytes(uc.mem_read(WIN, WIN_SIZE))
        return rx, rv, rn, rpc, mem


def fnv(b):
    h = 0xcbf29ce484222325
    for i in range(0, len(b), 8):
        h ^= struct.unpack_from('<Q', b, i)[0]
        h = (h * 0x100000001b3) & ((1 << 64) - 1)
    return h


# ----------------------------------------------------------------------------- main
def main():
    args = sys.argv[1:]
    per = int(args[args.index('--per') + 1]) if '--per' in args else 4
    seed = int(args[args.index('--seed') + 1]) if '--seed' in args else 1
    states = int(args[args.index('--states') + 1]) if '--states' in args else 3
    filt = args[args.index('--filter') + 1] if '--filter' in args else None
    rng = random.Random(seed)
    os.makedirs(WORK, exist_ok=True)
    prog = Program()
    tests = sample(prog, per, rng, filt, pairs='--pairs' in args)
    print('%d shapes sampled -> %d instructions' % (len({t[0] for t in tests}), len(tests)), flush=True)
    mem_init = bytes(rng.getrandbits(8) for _ in range(WIN_SIZE)) if WIN_SIZE <= 0x1000 else \
        random.Random(seed ^ 0x5eed).randbytes(WIN_SIZE)
    uni = Uni(prog, mem_init)
    envs = {n: TestEnv(prog, prog.mods[n]) for n in prog.mods}
    cases, funcs, expect = [], [], []
    lift_fail = collections.Counter()
    untested = collections.Counter()
    for key, mname, pc, words in tests:
        w = words[-1]
        env = envs[mname]
        fname = 't_%x' % pc
        try:
            src = lift_test(env, words, pc, fname)
        except Unimpl as e:
            lift_fail[str(e)] += 1
            continue
        good = 0
        for attempt in range(states * 6):
            if good >= states:
                break
            st = rand_state(rng)
            if w & 0xff9ffc1f == 0xd61f0000:              # BR, BLR, RET: branch somewhere mapped
                m = envs[mname].mod
                st[0][(w >> 5) & 31] = m.base + rng.randrange(m.text[0], m.text[1]) // 4 * 4
            try:
                res = uni.run(pc, st, words)
            except unicorn.UcError:
                continue
            cases.append((key, pc, words, fname, st))
            expect.append(res)
            good += 1
        if good:
            funcs.append(src)
        else:
            untested[key] += 1
    print('lifted %d, unicorn cases %d, lift failures %s' % (len(funcs), len(cases), dict(lift_fail)), flush=True)
    tested_keys = {c[0] for c in cases}
    never = sorted(k for k in untested if k not in tested_keys)
    print('shapes with no usable state (untested): %d %s' % (len(never), never[:40]), flush=True)
    write_c(cases, funcs, envs, mem_init)
    if not build():
        return
    out = os.path.join(WORK, 'results.bin')
    r = subprocess.run([os.path.join(WORK, 'a64test.exe'), os.path.join(WORK, 'states.bin'), out, os.path.join(WORK, 'mem.bin')],
                       capture_output=True, text=True)
    if r.returncode:
        print('test program failed', r.returncode, r.stdout[-2000:], r.stderr[-2000:])
        return
    compare(cases, expect, out)


def write_c(cases, funcs, envs, mem_init):
    with open(os.path.join(WORK, 'mem.bin'), 'wb') as f:
        f.write(mem_init)
    with open(os.path.join(WORK, 'states.bin'), 'wb') as f:
        for key, pc, w, fname, (x, sp, v, nzcv) in cases:
            f.write(struct.pack('<31QQ', *x, sp))
            for q in v:
                f.write(struct.pack('<QQ', q & ((1 << 64) - 1), q >> 64))
            f.write(struct.pack('<Q', nzcv))
    targets = set()
    for e in envs.values():
        targets |= e.targets
    names = sorted({c[3] for c in cases})
    with open(os.path.join(WORK, 'tests.c'), 'w') as f:
        f.write('#include "a64rt.h"\n#include <stdio.h>\n#include <stdlib.h>\n#include <windows.h>\n\n')
        for t in sorted(targets):
            f.write('static void tgt_%x(Ctx *c) { c->pc = 0x%xull; c->branched = 1; }\n' % (t, t))
        f.write('\n'.join(funcs) + '\n')
        f.write('typedef void (*TF)(Ctx *);\nstatic const TF cases[] = {\n')
        f.writelines('  %s,\n' % c[3] for c in cases)
        f.write('};\n#define NCASES %d\n' % len(cases))
        f.write(HARNESS % {'win': WIN, 'size': WIN_SIZE})


HARNESS = r'''
void call_ind(Ctx *c, u64 a) { c->pc = a; c->branched = 1; }
void jump_ind(Ctx *c) { c->branched = 1; }
void guest_trap(Ctx *c, const char *w, u64 pc) { c->pc = 0xdead0000ull; c->branched = 1; }
void guest_unimpl(Ctx *c, u64 pc, u32 insn, const char *w) { c->pc = 0xdead0001ull; c->branched = 1; }
void guest_svc(Ctx *c, u32 imm) {}
void host_yield(void) {}
u64 host_cntpct(void) { return 0; }
void host_set_fpcr(Ctx *c) {}

typedef struct { u64 x[32]; u64 v[64]; u64 nzcv; } State;
typedef struct { u64 x[32]; u64 v[64]; u64 nzcv, pc, memhash; } Result;

static u64 fnv(const u8 *b, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i += 8) { u64 w; memcpy(&w, b + i, 8); h ^= w; h *= 0x100000001b3ull; }
    return h;
}

int main(int argc, char **argv) {
    u8 *win = VirtualAlloc((void *)0x%(win)xull, 0x%(size)x, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (win != (u8 *)0x%(win)xull) { printf("cannot map the window\n"); return 2; }
    static u8 init[0x%(size)x];
    FILE *fm = fopen(argv[3], "rb"); fread(init, 1, sizeof init, fm); fclose(fm);
    FILE *fs = fopen(argv[1], "rb"), *fo = fopen(argv[2], "wb");
    for (int i = 0; i < NCASES; i++) {
        State s; Result r;
        fread(&s, sizeof s, 1, fs);
        Ctx *c = _aligned_malloc(sizeof(Ctx), 64);
        memset(c, 0, sizeof *c);
        memcpy(c->x, s.x, sizeof s.x);
        memcpy(c->v, s.v, sizeof s.v);
        c->nf = s.nzcv >> 3 & 1; c->zf = s.nzcv >> 2 & 1; c->cf = s.nzcv >> 1 & 1; c->vf = s.nzcv & 1;
        c->ex_addr = ~0ull;
        memcpy(win, init, sizeof init);
        __try { cases[i](c); } __except (1) { c->pc = 0xbadbad00ull | (GetExceptionCode() & 0xff); }
        memcpy(r.x, c->x, sizeof r.x);
        memcpy(r.v, c->v, sizeof r.v);
        r.nzcv = (u64)c->nf << 3 | (u64)c->zf << 2 | (u64)c->cf << 1 | c->vf;
        r.pc = c->pc;
        r.memhash = fnv(win, sizeof init);
        fwrite(&r, sizeof r, 1, fo);
        _aligned_free(c);
    }
    fclose(fo);
    return 0;
}
'''


def build():
    rt = os.path.join(ROOT, 'mk8recomp', 'runtime')
    bat = os.path.join(WORK, 'build.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\ncall "%s" >nul\ncd /d "%s"\n"%s" %s /I"%s" tests.c "%s" /Fea64test.exe /link /STACK:8388608\n' % (
            VCVARS, WORK, CLANG, ' '.join(CFLAGS), rt, os.path.join(rt, 'a64rt.c')))
    r = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    if r.returncode:
        print('build failed:\n' + (r.stdout + r.stderr)[-6000:])
        return False
    return True


def is_nan32(b):
    return (b & 0x7f800000) == 0x7f800000 and b & 0x7fffff


def is_nan64(b):
    return (b & 0x7ff0000000000000) == 0x7ff0000000000000 and b & 0xfffffffffffff


def compare(cases, expect, out):
    data = open(out, 'rb').read()
    rsz = 8 * (32 + 64 + 3)
    bad = collections.defaultdict(list)
    nan_only = collections.Counter()
    for i, ((key, pc, w, fname, st), (rx, rv, rn, rpc, mem)) in enumerate(zip(cases, expect)):
        r = struct.unpack_from('<32Q64Q3Q', data, i * rsz)
        hx_, hv, hn, hpc, hmem = r[:32], r[32:96], r[96], r[97], r[98]
        diffs = []
        for k in range(32):
            if hx_[k] != rx[k]:
                diffs.append('%s: %x != %x' % ('sp' if k == 31 else 'x%d' % k, hx_[k], rx[k]))
        nan_diff = False
        for k in range(32):
            h = hv[2 * k] | hv[2 * k + 1] << 64
            if h != rv[k]:
                # equal up to NaN payloads (lane-wise)?
                same = True
                for lane in range(4):
                    a, b = (h >> (32 * lane)) & 0xffffffff, (rv[k] >> (32 * lane)) & 0xffffffff
                    if a != b and not (is_nan32(a) and is_nan32(b)):
                        same = False
                if not same:
                    same = True
                    for lane in range(2):
                        a, b = (h >> (64 * lane)) & ((1 << 64) - 1), (rv[k] >> (64 * lane)) & ((1 << 64) - 1)
                        if a != b and not (is_nan64(a) and is_nan64(b)):
                            same = False
                if same:
                    nan_diff = True
                else:
                    diffs.append('v%d: %032x != %032x' % (k, h, rv[k]))
        if hn != rn:
            diffs.append('nzcv: %x != %x' % (hn, rn))
        if hpc != rpc:
            diffs.append('pc: %x != %x' % (hpc, rpc))
        if hmem != fnv(mem):
            diffs.append('memory differs')
        if diffs:
            bad[key].append((pc, w, diffs, st))
        elif nan_diff:
            nan_only[key] += 1
    nkeys = len({c[0] for c in cases})
    if nan_only:
        print('NaN-payload-only differences by shape:', dict(nan_only.most_common(20)))
    print('cases %d, shapes %d, failing shapes %d, failing cases %d, NaN-payload-only differences %d' % (
        len(cases), nkeys, len(bad), sum(len(v) for v in bad.values()), sum(nan_only.values())))
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for key in sorted(bad, key=lambda k: -len(bad[k])):
        pc, w, diffs, st = bad[key][0]
        dis = '; '.join('%s %s' % (i.mnemonic, i.op_str) for i in md.disasm(struct.pack('<%dI' % len(w), *w), pc))
        print('  %-50s %3d  e.g. %x %s %s: %s' % (key[:50], len(bad[key]), pc, ' '.join('%08x' % x for x in w), dis,
                                                  '; '.join(diffs[:4])))
    with open(os.path.join(WORK, 'failures.txt'), 'w') as f:
        for key in sorted(bad):
            for pc, w, diffs, st in bad[key]:
                x, sp, v, nzcv = st
                f.write('%s | %x %s | %s\n    x=%s sp=%x nzcv=%x\n' % (
                    key, pc, ' '.join('%08x' % x for x in w), '; '.join(diffs), ' '.join('%x' % r for r in x), sp, nzcv))


if __name__ == '__main__':
    main()
