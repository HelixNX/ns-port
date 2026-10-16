"""Static recompiler: AArch64 -> C, for the 64-bit modules of MK8DX 3.0.5 (main, subsdk0, sdk).

Design (see ../../../CLAUDE.md):
- every guest function becomes `void f_<address>(Ctx *c)`; the guest registers are C locals of that function
  (X0..X30, SP, V0..V31, the flags NF ZF CF VF), loaded from the Ctx on entry and stored back only at calls,
  returns and indirect branches (the per-function SAVE / LOAD macros), so clang can keep them in host registers
  and delete what is never used;
- guest memory is identity-mapped: a guest address is a host pointer (modules at fixed addresses, BASES);
- a call through a PLT stub is resolved here, across the three modules, to a direct call of the provider's
  lifted function (or of a host replacement `h_<symbol>`);
- an indirect branch looks the target up in a per-module table indexed by (address - text start) / 4;
- a switch (BR with a jump table that IDA resolved) becomes a C switch of gotos.

Instruction semantics live in a64insn.py (integer, branches, loads and stores, system) and a64simd.py (FP and SIMD).

Usage:
    python a64lift.py cover <module> [...]                 coverage statistics, nothing written
    python a64lift.py lift  <outdir> [--only mod:0xOFF,...] lift main, subsdk0 and sdk into <outdir>
Modules are read from ../../../dump_v21/elf/<module>.elf and ../../../build/analysis_305/<module>_ida.json.
"""
import collections
import io
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, HERE)

from nxmod import Module                       # noqa: E402
from a64insn import Unimpl, lift_insn, branch_targets   # noqa: E402

# load order = symbol lookup order (as rtld does it: main, then the subsdks, then sdk)
MODULES = ['main', 'subsdk0', 'sdk']
BASES = {'main': 0x7100000000, 'subsdk0': 0x7101400000, 'sdk': 0x7101c00000}

NREG = 32          # X0..X30 and SP (31)
LINE_INFO = os.environ.get('LIFT_LINES', '1') != '0'   # #line <guest offset> before every instruction


def regname(n):
    return 'SP' if n == 31 else 'X%d' % n


# ----------------------------------------------------------------------------- modules and symbols
class Mod:
    """One module: image, IDA functions, PLT stubs."""

    def __init__(self, name, elf=None, ida=None):
        self.name = name
        self.m = Module(elf or os.path.join(ROOT, 'dump_v21', 'elf', name + '.elf'))
        ida = json.load(open(ida or os.path.join(ROOT, 'build', 'analysis_305', name + '_ida.json')))
        self.base = BASES[name]
        self.text = self.m.text
        data = set()
        for s, e in ida['text_data']:
            data.update(range(s & ~3, e, 4))
        self.funcs = {}                      # entry offset -> sorted instruction offsets
        self.owner = {}                      # instruction offset -> function entry offset
        for f in ida['functions']:
            addrs = sorted({a for s, e in f['chunks'] for a in range(s, e, 4) if a not in data})
            if not addrs:
                continue
            self.funcs[f['ea']] = addrs
            for a in addrs:
                self.owner.setdefault(a, f['ea'])
        self.switches = {int(k, 16): v for k, v in ida.get('switches', {}).items()}
        self.addr_refs = code_address_refs(self)
        self.recover_functions(data)
        got = {r[0]: r[2] for r in self.m.rels if r[1] in (1025, 1026)}      # GLOB_DAT / JUMP_SLOT slot -> symbol
        self.plt = {}                        # PLT stub offset -> symbol name
        for ea in self.funcs:
            slot = self.plt_slot(ea)
            if slot is not None and slot in got:
                self.plt[ea] = self.m.syms[got[slot]][0]
        self.exports = {s[0]: s[1] for s in self.m.exports() if s[3] & 0xf in (1, 2, 0)}

    def word(self, off):
        return self.m.u32(off)

    def recover_functions(self, data):
        """Code that IDA left out of every function (its analysis of main's first 0x500 bytes is broken: a
        virtual method at main+1f4, instructions classed as data): address-taken code, direct branch and call
        targets, and fall-through past a function's last instruction. Each becomes a function by following its
        control flow, through IDA's "data" if the flow leads there, up to returns, indirect branches and other
        functions' code; repeated until nothing new turns up."""
        t0, t1 = self.text
        seeds = set(self.m.code_seeds()) | self.addr_refs
        todo = set(self.funcs)
        while True:
            # direct branch / call targets in no function, and fall-through past a function's last instruction
            for f in todo:
                offs = self.funcs[f]
                for i, a in enumerate(offs):
                    w = self.word(a)
                    seeds.update(t - self.base for t in branch_targets(w, self.base + a))
                    if (i + 1 == len(offs) or offs[i + 1] != a + 4) and w & 0xfffffc1f not in (0xd65f0000, 0xd61f0000) \
                            and w & 0xfc000000 != 0x14000000 and w & 0xffe0001f != 0xd4200000 and w != 0 \
                            and w != 0x94000002:                    # BL over an inline constant: data follows
                        seeds.add(a + 4)
            new = self.recover_from(seeds, data)
            if not new:
                break
            todo, seeds = new, set()

    def recover_from(self, seeds, data):
        t0, t1 = self.text
        new = []
        for seed in sorted(seeds):
            if not (t0 <= seed < t1) or seed & 3 or seed in self.owner or self.word(seed) == 0:
                continue
            addrs, work = set(), [seed]
            while work:
                a = work.pop()
                if a in addrs or not (t0 <= a < t1) or (a in self.owner and a != seed):
                    continue
                w = self.word(a)
                if w == 0 or w & 0xffe0001f == 0xd4200000:            # UDF / BRK: not code, or a trap
                    addrs.add(a)
                    continue
                addrs.add(a)
                work.extend(t - self.base for t in branch_targets(w, self.base + a) if w & 0xfc000000 != 0x94000000)
                if w & 0xfffffc1f in (0xd65f0000, 0xd61f0000) or w & 0xfc000000 == 0x14000000:
                    continue                                         # RET, BR, B: no fall-through
                work.append(a + 4)
            order = sorted(addrs)
            self.funcs[seed] = order
            new.append(seed)
            for a in order:
                self.owner.setdefault(a, seed)
            print('%s: recovered function %x (%d instructions%s)' % (
                self.name, seed, len(order), ', through IDA data' if data & addrs else ''))
        return new

    def plt_slot(self, ea):
        """GOT slot of `adrp x16, p; ldr x17, [x16, #o]; add x16, x16, #o; br x17`, or None."""
        if ea + 16 > self.text[1]:
            return None
        w0, w1, w2, w3 = (self.word(ea + 4 * i) for i in range(4))
        if w0 & 0x9f00001f != 0x90000010 or w1 & 0xffc003ff != 0xf9400211 or w3 != 0xd61f0220:
            return None
        immlo, immhi = (w0 >> 29) & 3, (w0 >> 5) & 0x7ffff
        page = ((immhi << 2) | immlo)
        if page & (1 << 20):
            page -= 1 << 21
        return (ea & ~0xfff) + (page << 12) + ((w1 >> 10) & 0xfff) * 8


class Program:
    """All three modules, with imports resolved between them."""

    def __init__(self, names=MODULES, overrides=None):
        self.mods = {n: Mod(n) for n in names}
        self.overrides = overrides or set()       # symbols replaced by host code: calls become h_<symbol>(c)
        self.sym_addr = {}                        # symbol -> absolute address of its definition
        for n in MODULES:
            if n in self.mods:
                for s, v in self.mods[n].exports.items():
                    if s not in self.sym_addr and v:
                        self.sym_addr[s] = self.mods[n].base + v

    def mod_of(self, addr):
        for m in self.mods.values():
            if m.base + m.text[0] <= addr < m.base + m.text[1]:
                return m
        return None


def mangle(sym):
    return ''.join(ch if ch.isalnum() or ch == '_' else '_%02x' % ord(ch) for ch in sym)


# ----------------------------------------------------------------------------- one function
class Fn:
    """Lifting state for one guest function (all its chunks), entered at `entry` (absolute)."""

    def __init__(self, env, mod, entry, offs):
        self.env, self.mod = env, mod
        self.entry = entry
        self.addrs = {mod.base + o for o in offs}
        self.order = [mod.base + o for o in offs]
        self.rx, self.wx = set(), set()           # X registers (31 = SP) read / written
        self.rv, self.wv = set(), set()           # V registers read / written
        self.flags = False                        # flags used at all
        self.labels = set()                       # addresses that need a label
        self.temps = set()                        # extra function-scope locals ('FA', 'FB', 'JT')
        self.fk = None                            # what the flags were last computed from (see a64insn.cond)
        self.extra_decls = []

    # ---- registers
    def x(self, n, sp=False):
        """64-bit read of Xn; n == 31 is SP when sp, else XZR."""
        if n == 31 and not sp:
            return '0ull'
        self.rx.add(n)
        return regname(n)

    def w(self, n, sp=False):
        if n == 31 and not sp:
            return '0u'
        self.rx.add(n)
        return '(u32)' + regname(n)

    def r(self, n, sf, sp=False):
        return self.x(n, sp) if sf else self.w(n, sp)

    def setx(self, n, expr, sp=False):
        if n == 31 and not sp:
            return '(void)(%s);' % expr
        self.wx.add(n)
        return '%s = %s;' % (regname(n), expr)

    def setw(self, n, expr, sp=False):
        if n == 31 and not sp:
            return '(void)(%s);' % expr
        self.wx.add(n)
        return '%s = (u32)(%s);' % (regname(n), expr)

    def setr(self, n, sf, expr, sp=False):
        return self.setx(n, expr, sp) if sf else self.setw(n, expr, sp)

    def v(self, n):
        self.rv.add(n)
        return 'V%d' % n

    def setv(self, n, expr):
        self.wv.add(n)
        return 'V%d = %s;' % (n, expr)

    def use_flags(self):
        self.flags = True

    def temp(self, name):
        self.temps.add(name)
        return name

    # ---- control flow
    def save(self):
        return 'SAVE;'

    def call(self, target, ret_addr):
        """BL target: set LR, call, reload."""
        self.wx.add(30)
        self.fk = None
        return 'X30 = 0x%xull; SAVE; %s(c); LOAD;' % (ret_addr, self.env.callee(target))

    def call_ind(self, expr, ret_addr):
        self.wx.add(30)
        self.fk = None
        return '{ u64 t = %s; X30 = 0x%xull; SAVE; call_ind(c, t); LOAD; }' % (expr, ret_addr)

    def jump(self, target):
        """Unconditional direct branch."""
        if target in self.addrs:
            self.labels.add(target)
            return 'goto L_%x;' % target
        return '{ SAVE; MUSTTAIL return %s(c); }' % self.env.callee(target)

    def jump_ind(self, expr, at):
        """BR Xn: a resolved switch becomes gotos; anything else is an indirect tail call."""
        cases = self.mod.switches.get(at - self.mod.base)
        if cases:
            self.temp('JT')
            out = ['{ JT = %s; switch (JT) {' % expr]
            for t in cases:
                ta = self.mod.base + t
                if ta in self.addrs:
                    self.labels.add(ta)
                    out.append('case 0x%xull: goto L_%x;' % (ta, ta))
            out.append('default: c->pc = JT; SAVE; MUSTTAIL return jump_ind(c); } }')
            return ' '.join(out)
        return '{ c->pc = %s; SAVE; MUSTTAIL return jump_ind(c); }' % expr

    def ret(self, expr):
        return '{ SAVE; return; }'

    def svc(self, n):
        self.fk = None
        return 'SAVE; guest_svc(c, 0x%x); LOAD;' % n


class Env:
    """Lifting of one module within the program; collects secondary entries and callees."""

    def __init__(self, prog, mod):
        self.prog, self.mod = prog, mod
        self.extra = set()                 # absolute addresses lifted as secondary entries of this module
        self.ext_callees = set()           # names of functions defined elsewhere (other modules, host)
        self.unresolved = collections.Counter()

    def callee(self, target):
        """C function name for a direct call or tail call to absolute address `target`."""
        mod = self.mod
        off = target - mod.base
        if off in mod.plt:
            sym = mod.plt[off]
            if sym in self.prog.overrides:
                name = 'h_' + mangle(sym)
            elif sym in self.prog.sym_addr:
                name = 'f_%x' % self.prog.sym_addr[sym]
            else:
                self.unresolved[sym] += 1
                name = 'u_' + mangle(sym)
            self.ext_callees.add(name)
            return name
        if not (mod.text[0] <= off < mod.text[1]):
            m2 = self.prog.mod_of(target)
            if m2 is None:
                raise Unimpl('branch-outside-text')
            name = 'f_%x' % target
            self.ext_callees.add(name)
            return name
        if off not in mod.funcs:
            self.extra.add(target)
        return 'f_%x' % target


def secondary_entries(mod):
    """Direct branch and call targets that are inside a function but not at its start, by owning function
    (offsets); and the targets that are in no function at all (orphans)."""
    sec, orphans = collections.defaultdict(set), set()
    for off, offs in mod.funcs.items():
        if off in mod.plt:
            continue
        own = set(offs)
        for a in offs:
            w = mod.word(a)
            if w & 0xfc000000 == 0x94000000:                          # BL
                targets, call = (a + (sext26(w) << 2),), True
            else:
                targets, call = tuple(t - mod.base for t in branch_targets(w, mod.base + a)), False
            for t in targets:
                if not (mod.text[0] <= t < mod.text[1]) or t in mod.funcs or (not call and t in own):
                    continue
                o = mod.owner.get(t)
                if o is None:
                    orphans.add(t)
                else:
                    sec[o].add(t)
    # code whose address is taken (relocation targets, exports, init_array, ADR / ADRP+ADD) and that IDA put
    # inside another function: it is entered through call_ind, so it needs an entry too
    for t in set(mod.m.code_seeds()) | mod.addr_refs:
        if t in mod.funcs or not (mod.text[0] <= t < mod.text[1]):
            continue
        o = mod.owner.get(t)
        if o is not None:
            sec[o].add(t)
    return {o: sorted(v) for o, v in sec.items()}, orphans


def code_address_refs(mod):
    """Text addresses computed by ADR, or by ADRP followed by ADD to the same register."""
    def sx(v, b):
        return v - (1 << b) if v >> (b - 1) & 1 else v
    out = set()
    t0, t1 = mod.text
    for a in range(t0, t1 - 4, 4):
        w = mod.word(a)
        if w & 0x9f000000 == 0x10000000:                               # ADR
            t = a + sx(((w >> 5) & 0x7ffff) << 2 | (w >> 29) & 3, 21)
        elif w & 0x9f000000 == 0x90000000:                             # ADRP, then ADD from that register
            rd = w & 31
            page = (a & ~0xfff) + (sx(((w >> 5) & 0x7ffff) << 2 | (w >> 29) & 3, 21) << 12)
            for b in range(a + 4, min(a + 4 * 32, t1), 4):
                w2 = mod.word(b)
                if w2 & 0xffc00000 == 0x91000000 and (w2 >> 5) & 31 == rd:
                    t = page + ((w2 >> 10) & 0xfff)
                    if t0 <= t < t1 and not t & 3:
                        out.add(t)
                if w2 & 0xfc000000 in (0x14000000, 0x94000000) or w2 & 0xfffffc1f == 0xd65f0000 or \
                        (w2 & 0x9f000000 == 0x90000000 and w2 & 31 == rd):
                    break             # B, BL, RET or a new ADRP to rd end the search (extra hits only add entries)
            continue
        else:
            continue
        if t0 <= t < t1 and not t & 3:
            out.add(t)
    return out


def sext26(w):
    v = w & 0x3ffffff
    return v - (1 << 26) if v & (1 << 25) else v


def lift_function(env, owner, secondary=(), stats=None):
    """C source of the function starting at offset `owner`, with its secondary entries (offsets)."""
    mod = env.mod
    offs = mod.funcs[owner]
    entry = mod.base + owner
    fn = Fn(env, mod, entry, offs)
    entries = [entry] + [mod.base + e for e in secondary]
    # labels for every intra-function branch target (also resets what the flags are known to hold)
    bt = set()
    for a in fn.order:
        for t in branch_targets(mod.word(a - mod.base), a):
            if t in fn.addrs:
                bt.add(t)
    sw_targets = set()
    for br, cases in mod.switches.items():
        if mod.base + br in fn.addrs:
            sw_targets.update(mod.base + t for t in cases)
    bt |= sw_targets
    bt |= set(entries)
    fn.labels.update(entries)
    lines = []
    prev = None
    noreturn_prev = False

    def falls_through(stmt):
        return not (stmt.startswith('goto ') or stmt.endswith(('return; }', 'return; } }')) or stmt.startswith('{ SAVE; return'))

    for pc in fn.order:
        if prev is not None and pc != prev + 4 and not noreturn_prev:
            lines.append((None, fn.jump(prev + 4) if (prev + 4 in fn.addrs or env_is_entry(env, prev + 4))
                          else 'guest_trap(c, "ran past the end of a chunk", 0x%xull); return;' % (prev + 4)))
        if pc in bt:
            fn.fk = None
            fn.labels.add(pc)
        w = mod.word(pc - mod.base)
        try:
            stmt = lift_insn(fn, w, pc)
            if stats is not None:
                stats['ok'] += 1
        except Unimpl as e:
            stmt = 'SAVE; guest_unimpl(c, 0x%xull, 0x%08xu, "%s"); LOAD;' % (pc, w, e)
            fn.fk = None
            if stats is not None:
                stats['bad'][str(e)] += 1
                stats['example'].setdefault(str(e), (pc, w))
        lines.append((pc, stmt))
        noreturn_prev = not falls_through(stmt)
        prev = pc
    if not noreturn_prev:
        nxt = prev + 4
        lines.append((None, fn.jump(nxt) if (nxt in fn.addrs or env_is_entry(env, nxt))
                      else 'guest_trap(c, "ran past the end of a function", 0x%xull); return;' % nxt))
    return emit(fn, lines, entries)


def env_is_entry(env, addr):
    off = addr - env.mod.base
    return off in env.mod.funcs


def emit(fn, lines, entries=None):
    """The C function. With several entries, the body is b_<entry> and each entry f_<addr> is a wrapper that
    tail-calls it with c->entry set to the entry's index."""
    entries = entries or [fn.entry]
    used_x = sorted(fn.rx | fn.wx)
    used_v = sorted(fn.rv | fn.wv)
    multi = len(entries) > 1
    out = [('static void b_%x(Ctx *c) {' if multi else 'void f_%x(Ctx *c) {') % fn.entry]
    if used_x:
        out.append('  u64 ' + ', '.join('%s = c->x[%d]' % (regname(n), n) for n in used_x) + ';')
    if used_v:
        out.append('  v128 ' + ', '.join('V%d = c->v[%d]' % (n, n) for n in used_v) + ';')
    if fn.flags:
        out.append('  u32 NF = c->nf, ZF = c->zf, CF = c->cf, VF = c->vf;')
    itemps = sorted(t for t in fn.temps if not t.startswith('FF'))
    ftemps = sorted(t for t in fn.temps if t.startswith('FF'))
    if itemps:
        out.append('  u64 ' + ', '.join('%s = 0' % t for t in itemps) + ';')
    if ftemps:
        out.append('  f64 ' + ', '.join('%s = 0' % t for t in ftemps) + ';')
    out.extend('  ' + d for d in fn.extra_decls)
    save = ['c->x[%d] = %s;' % (n, regname(n)) for n in sorted(fn.wx)]
    save += ['c->v[%d] = V%d;' % (n, n) for n in sorted(fn.wv)]
    load = ['%s = c->x[%d];' % (regname(n), n) for n in used_x]
    load += ['V%d = c->v[%d];' % (n, n) for n in used_v]
    if fn.flags:
        save.append('c->nf = NF; c->zf = ZF; c->cf = CF; c->vf = VF;')
        load.append('NF = c->nf; ZF = c->zf; CF = c->cf; VF = c->vf;')
    out.append('#define SAVE do { %s } while (0)' % ' '.join(save))
    out.append('#define LOAD do { %s } while (0)' % ' '.join(load))
    if multi:
        out.append('  switch (c->entry) { %s}' % ''.join('case %d: goto L_%x; ' % (i, e) for i, e in enumerate(entries)
                                                          if i or fn.order[0] != e))
    elif fn.order[0] != fn.entry:
        out.append('  goto L_%x;' % fn.entry)
    base = fn.mod.base
    out.append('#line %d "%s"' % (fn.order[0] - base, fn.mod.name))
    for pc, stmt in lines:
        if pc is not None and LINE_INFO:
            out.append('#line %d' % (pc - base))       # guest offset as the line number: crash reports name it
        if pc is not None and pc in fn.labels:
            out.append('L_%x: %s' % (pc, stmt))
        else:
            out.append('  ' + stmt)
    out.append('#undef SAVE')
    out.append('#undef LOAD')
    out.append('}')
    if multi:
        for i, e in enumerate(entries):
            out.append('void f_%x(Ctx *c) { c->entry = %d; MUSTTAIL return b_%x(c); }' % (e, i, fn.entry))
    return '\n'.join(out)


# ----------------------------------------------------------------------------- commands
def cmd_cover(names):
    prog = Program()
    for n in names:
        mod = prog.mods[n]
        env = Env(prog, mod)
        stats = {'ok': 0, 'bad': collections.Counter(), 'example': {}}
        sec, orphans = secondary_entries(mod)
        nf = 0
        for off in sorted(mod.funcs):
            if off in mod.plt:
                continue
            lift_function(env, off, sec.get(off, ()), stats)
            nf += 1
        total = stats['ok'] + sum(stats['bad'].values())
        print('%s: functions %d (+%d PLT stubs)  instructions %d  translated %d (%.3f%%)  secondary entries %d  '
              'unresolved imports %d' % (n, nf, len(mod.plt), total, stats['ok'], 100.0 * stats['ok'] / max(total, 1),
                                         len(env.extra), len(env.unresolved)))
        for r, k in stats['bad'].most_common(60):
            pc, w = stats['example'][r]
            print('  %-28s %7d   e.g. %x: %08x' % (r, k, pc, w))
        if env.unresolved:
            print('  unresolved:', ', '.join(sorted(env.unresolved)[:20]))


def cmd_lift(outdir, only=None, per_file=40000):
    prog = Program(overrides=load_overrides())
    os.makedirs(outdir, exist_ok=True)
    stats = {'ok': 0, 'bad': collections.Counter(), 'example': {}}
    all_entries = {}
    all_files = []
    ext = set()
    for n in MODULES:
        mod = prog.mods[n]
        env = Env(prog, mod)
        sec, orphans = secondary_entries(mod)
        if only:
            todo = sorted(o for o in mod.funcs if mod.base + o in only)
        else:
            todo = sorted(o for o in mod.funcs if o not in mod.plt)
        done, files, buf, count = set(), [], [], 0

        def flush():
            nonlocal buf, count
            if buf:
                path = os.path.join(outdir, '%s_%03d.c' % (n, len(files)))
                write_if_changed(path, '/* generated by a64lift.py -- do not edit */\n#include "a64rt.h"\n'
                                       '#include "funcs.h"\n\n' + '\n\n'.join(buf) + '\n')
                files.append(path)
                buf, count = [], 0

        for off in todo:
            src = lift_function(env, off, sec.get(off, ()), stats)
            done.add(mod.base + off)
            done.update(mod.base + e for e in sec.get(off, ()))
            buf.append(src)
            count += src.count('\n') + 1
            if count >= per_file:
                flush()
        # branch targets in no function, and anything the pre-pass missed: trap if ever entered
        for a in sorted(({mod.base + o for o in orphans} | env.extra) - done):
            buf.append('void f_%x(Ctx *c) { guest_trap(c, "entry outside every function", 0x%xull); }' % (a, a))
            done.add(a)
        if env.extra - done:
            print('%s: %d secondary entries missed by the pre-pass' % (n, len(env.extra - done)))
        flush()
        all_files.extend(files)
        all_entries[n] = sorted(done)
        ext |= env.ext_callees
        if env.unresolved:
            print('%s: %d unresolved imports, e.g. %s' % (n, len(env.unresolved), ', '.join(sorted(env.unresolved)[:8])))
        print('%s: %d files, %d functions' % (n, len(files), len(done)))
    defined = {'f_%x' % e for es in all_entries.values() for e in es}
    with Out(os.path.join(outdir, 'funcs.h')) as f:
        f.write('/* generated by a64lift.py */\n#pragma once\n')
        for e in sorted(defined):
            f.write('void %s(Ctx *c);\n' % e)
        for e in sorted(ext - defined):
            f.write('void %s(Ctx *c);\n' % e)
    with Out(os.path.join(outdir, 'table.c')) as f:
        f.write('/* generated by a64lift.py: lifted entry points, per module, for the indirect-branch tables */\n')
        f.write('#include "a64rt.h"\n#include "funcs.h"\n')
        for n, es in all_entries.items():
            f.write('const FnEntry fn_entries_%s[] = {\n' % n)
            f.writelines('  {0x%xull, f_%x},\n' % (e, e) for e in es)
            f.write('  {0, 0}\n};\n')
        f.write('const ModInfo lifted_modules[] = {\n')
        for n, es in all_entries.items():
            m = prog.mods[n]
            f.write('  {"%s", 0x%xull, 0x%xull, 0x%xull, fn_entries_%s},\n' % (n, m.base, m.base + m.text[1], m.m.size, n))
        f.write('  {0}\n};\n')
        missing = sorted(x for x in ext - defined if x.startswith('f_'))
        f.write('/* %d cross-module callees not lifted as entries */\n' % len(missing))
    written = {os.path.basename(p) for p in all_files} | {'funcs.h', 'table.c'}
    for name in os.listdir(outdir):
        if name.endswith(('.c', '.h')) and name not in written:
            os.remove(os.path.join(outdir, name))
            print('removed stale', name)
    total = stats['ok'] + sum(stats['bad'].values())
    print('instructions %d, not translated %d' % (total, total - stats['ok']))


def write_if_changed(path, text):
    """Keep the old file (and its time stamp) when nothing changed, so that a re-lift rebuilds only what changed."""
    try:
        if open(path).read() == text:
            return
    except OSError:
        pass
    with open(path, 'w') as f:
        f.write(text)


class Out(io.StringIO):
    def __init__(self, path):
        super().__init__()
        self.path = path

    def __exit__(self, *exc):
        if exc[0] is None:
            write_if_changed(self.path, self.getvalue())
        return super().__exit__(*exc)


def load_overrides():
    path = os.path.join(HERE, 'overrides.txt')
    if not os.path.exists(path):
        return set()
    return {ln.split('#')[0].strip() for ln in open(path) if ln.split('#')[0].strip()}


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if sys.argv[1] == 'cover':
        cmd_cover(sys.argv[2:] or MODULES)
    elif sys.argv[1] == 'lift':
        only = None
        if '--only' in sys.argv:
            only = {int(x, 16) for x in sys.argv[sys.argv.index('--only') + 1].split(',')}
        cmd_lift(sys.argv[2], only)
    else:
        raise SystemExit(__doc__)
