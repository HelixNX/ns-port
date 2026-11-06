"""Static recompiler: AArch64 -> C, for the modules of a Switch game (game.json, see ../gamecfg.py).

Design (see the game's CLAUDE.md, e.g. mk8dx/CLAUDE.md):
- every guest function becomes `void f_<address>(Ctx *c)`; the guest registers are C locals of that function
  (X0..X30, SP, V0..V31, the flags NF ZF CF VF), loaded from the Ctx on entry and stored back only at calls,
  returns and indirect branches (the per-function SAVE / LOAD macros), so clang can keep them in host registers
  and delete what is never used;
- guest memory is identity-mapped: a guest address is a host pointer (modules at fixed addresses, BASES);
- a call through a PLT stub is resolved here, across the modules, to a direct call of the provider's
  lifted function (or of a host replacement `h_<symbol>`);
- an indirect branch looks the target up in a per-module table indexed by (address - text start) / 4;
- a switch (BR with a jump table that IDA resolved) becomes a C switch of gotos.

Instruction semantics live in a64insn.py (integer, branches, loads and stores, system) and a64simd.py (FP and SIMD).

Usage:
    python a64lift.py cover <module> [...]                 coverage statistics, nothing written
    python a64lift.py lift  <outdir> [--only mod:0xOFF,...] lift every module into <outdir>
Lifting runs in NX_LIFT_PROCS worker processes (default: by free memory, ~3 GB each, at most 8); 1 lifts in this one.
Modules are read from <dump>/elf/<module>.elf and <analysis>/<module>_ida.json (game.json): an IDA / Ghidra export
(ida_export.py, tools/ghidra), or, when there is none, written from the module's unwind tables (../funcs.py: the same
lift without IDA or Ghidra, as players build it).
"""
import collections
import concurrent.futures
import ctypes
import io
import json
import os
import pickle
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, HERE)

import gamecfg                                  # noqa: E402
from nxmod import Module                       # noqa: E402
from a64insn import Unimpl, lift_insn, branch_targets, sext, decode_bitmask   # noqa: E402

# load order = symbol lookup order (as rtld does it: main, then the subsdks, then sdk)
MODULES = gamecfg.MODULES
BASES = gamecfg.BASES

NREG = 32          # X0..X30 and SP (31)
LINE_INFO = os.environ.get('LIFT_LINES', '1') != '0'   # #line <guest offset> before every instruction


def regname(n):
    return 'SP' if n == 31 else 'X%d' % n


# ----------------------------------------------------------------------------- modules and symbols
class Mod:
    """One module: image, IDA functions, PLT stubs."""

    def __init__(self, name, elf=None, ida=None):
        self.name = name
        elf = elf or os.path.join(gamecfg.ELF, name + '.elf')
        ida = ida or os.path.join(gamecfg.ANALYSIS, name + '_ida.json')
        if not os.path.exists(ida):         # no IDA / Ghidra export: the module's own unwind tables (funcs.py)
            import funcs
            os.makedirs(os.path.dirname(ida), exist_ok=True)
            funcs.export(elf, ida)
        self.m = Module(elf)
        self.base = BASES[name]
        self.text = self.m.text
        # the analysis below takes minutes for a large module: cached, keyed by its inputs and this source
        key = repr([(p, os.path.getsize(p), os.path.getmtime(p)) for p in (elf, ida, __file__)])
        cache = os.path.join(gamecfg.BUILD, 'modcache', name + '.pickle')
        try:
            with open(cache, 'rb') as f:
                k, state = pickle.load(f)
            if k == key:
                self.__dict__.update(state)
                return
        except (OSError, EOFError, pickle.UnpicklingError, ValueError):
            pass
        self.analyse(json.load(open(ida)))
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with open(cache, 'wb') as f:
            pickle.dump((key, {k: v for k, v in self.__dict__.items() if k not in ('m', '_bt_cache')}), f,
                        pickle.HIGHEST_PROTOCOL)

    def analyse(self, ida):
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
        # case code a recovered table adds can call functions no one else calls (Ghidra leaves more tables
        # unresolved than IDA did: main+ce60's case code is the only caller of main+14260)
        while self.recover_switches():
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
                    if w & 0xfc000000 == 0x94000000:                    # BL (branch_targets leaves calls out)
                        seeds.add(a + (sext(w & 0x3ffffff, 26) << 2))
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

    def recover_switches(self):
        """Jump tables IDA did not resolve (a second dispatch on an index already bounds-checked earlier has no
        bounds check of its own): clang's
            adrp xT, tab; add xT, xT, #lo; adr xB, base; ldr{b,h,sh,sw,w} wE, [xT, xI{, lsl #k}];
            add xD, xB, xE{, lsl #s | sxtw #s}; br xD
        Definitions are tracked over the 512 words before the BR in address order, whoever owns them (the table
        address is often loaded once before a loop, sometimes in a part IDA split off), any other write forgetting
        them. Entries are read while the target is
        in the function, in no function at all, or nearby in another function (an entry there); reading a few too many (into the next table) only adds labels, and
        a target outside the cases still takes the dynamic path. Case code in no function (IDA never reached it) is
        added to the function, which can bring more tables: repeated until nothing changes."""
        found = grown = 0
        todo = set(self.funcs)
        while todo:
            changed = set()
            for f in sorted(todo):
                offs = self.funcs[f]
                inside = set(offs)
                for i, a in enumerate(offs):
                    w = self.word(a)
                    if w & 0xfffffc1f != 0xd61f0000 or a in self.switches:
                        continue
                    cases = (self.jump_table(range(max(self.text[0], a - 0x800), a, 4), (w >> 5) & 31, inside) or
                             self.jump_table(range(max(self.text[0], a - 0x4000), a, 4), (w >> 5) & 31, inside) or
                             self.jump_table(range(max(self.text[0], a - 0x40), a, 4), (w >> 5) & 31, inside,
                                             self.table_candidates(offs)))
                    if not cases:
                        continue
                    self.switches[a] = cases
                    found += 1
                    new = self.flow({t for t in cases if t not in inside and t not in self.owner})
                    if new:
                        inside |= new
                        for t in new:
                            self.owner.setdefault(t, f)
                        grown += len(new)
                        changed.add(f)
                if f in changed:
                    self.funcs[f] = sorted(inside)
            todo = changed
        if found:
            print('%s: %d jump tables found beyond IDA\'s (%d instructions of case code added)' % (self.name, found, grown))
        return found

    def table_candidates(self, offs):
        """(start, end, addresses in the read-only segment the function builds with ADRP + ADD) for jump_table"""
        page, addrs = {}, set()
        for a in offs:
            w = self.word(a)
            if w & 0x9f000000 == 0x90000000:                                    # ADRP
                imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
                page[w & 31] = (a & ~0xfff) + (sext(imm, 21) << 12)
            elif w & 0xff800000 == 0x91000000 and (w >> 5) & 31 in page:       # ADD (immediate), 64-bit
                c = page[(w >> 5) & 31] + (((w >> 10) & 0xfff) << (12 if w & (1 << 22) else 0))
                if self.m.ro[0] <= c < self.m.ro[1]:
                    addrs.add(c)
        return offs[0], offs[-1] + 4, sorted(addrs)

    def flow(self, starts):
        """Instructions reachable from starts without entering any function's code (as recover_from walks)."""
        t0, t1 = self.text
        addrs, work = set(), list(starts)
        while work:
            a = work.pop()
            if a in addrs or not (t0 <= a < t1) or a in self.owner:
                continue
            w = self.word(a)
            addrs.add(a)
            if w == 0 or w & 0xffe0001f == 0xd4200000:
                continue
            work.extend(t - self.base for t in branch_targets(w, self.base + a) if w & 0xfc000000 != 0x94000000)
            if w & 0xfffffc1f in (0xd65f0000, 0xd61f0000) or w & 0xfc000000 == 0x14000000:
                continue
            work.append(a + 4)
        return addrs

    def jump_table(self, before, reg, inside, candidates=None):
        """The cases of `br reg` after the instructions `before` (module offsets, in order), or None. The number of
        entries comes from the index's bound when one is seen (AND with a low mask, UBFX / UXTB / UXTH; a CMP #n
        shortly before the BR, guarding B.HI); without one, up to 256 entries are read while their targets stay
        within 16 KB. When the table's address is not known from the instructions before (another path set it: the
        register holds different values on different paths, as in main+280318), `candidates` (start, end, addresses)
        gives the function's bounds and every address it builds with ADRP + ADD: each one whose entries (a known
        bound needed) all land inside the function is taken, their cases joined (an extra label is harmless; a
        missing one is a guest trap at run time)."""
        defs = {}                                   # register -> (kind, ...), from the instructions before the BR
        bounds = {}                                 # register -> entries its value can index (AND mask)
        last_cmp = None                             # (offset, n + 1) of the last CMP #n
        cmps = []                                   # (offset, register, n + 1) of every CMP / CCMP #n
        before = list(before)
        for a in before:
            w = self.word(a)
            rd = w & 31
            if w & 0x9f000000 == 0x90000000:                                    # ADRP
                imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
                defs[rd] = ('addr', (a & ~0xfff) + (sext(imm, 21) << 12))
            elif w & 0x9f000000 == 0x10000000:                                  # ADR
                imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
                defs[rd] = ('addr', a + sext(imm, 21))
            elif w & 0xff800000 == 0x91000000:                                  # ADD (immediate), 64-bit
                src = defs.get((w >> 5) & 31)
                imm = ((w >> 10) & 0xfff) << (12 if w & (1 << 22) else 0)
                defs[rd] = ('addr', src[1] + imm) if src and src[0] == 'addr' else None
            elif w & 0x3f200c00 == 0x38200800 and (w >> 22) & 3:               # LDR, LDRS* (register offset)
                size, opc = w >> 30, (w >> 22) & 3
                signed = opc >= 2
                if size == 3 or (size == 2 and opc == 3):
                    defs[rd] = None
                    continue
                tab = defs.get((w >> 5) & 31)
                idx = (w >> 16) & 31
                # a table address not known here (loaded on another path) is guessed below (candidates)
                defs[rd] = ('entry', tab[1] if tab and tab[0] == 'addr' else None, 1 << size, signed, bounds.get(idx),
                            idx)
            elif w & 0xff200000 == 0x8b000000 and (w >> 22) & 3 == 0:          # ADD (shifted register), LSL
                defs[rd] = self._table_sum(defs, (w >> 5) & 31, (w >> 16) & 31, (w >> 10) & 63)
            elif w & 0xffe00000 == 0x8b200000 and (w >> 13) & 7 in (2, 6):    # ADD (extended register), UXTW/SXTW
                defs[rd] = self._table_sum(defs, (w >> 5) & 31, (w >> 16) & 31, (w >> 10) & 7)
            else:                                   # anything else: forget what it writes
                if w & 0x7f80001f == 0x7100001f:                                # CMP (immediate)
                    last_cmp = (a, (((w >> 10) & 0xfff) << (12 if w & (1 << 22) else 0)) + 1)
                    cmps.append((a, (w >> 5) & 31, last_cmp[1]))
                elif w & 0x7fe00c10 == 0x7a400800:                              # CCMP (immediate)
                    cmps.append((a, (w >> 5) & 31, ((w >> 16) & 31) + 1))
                for r in self._writes(w):
                    defs[r] = None
                    bounds.pop(r, None)
                if w & 0x7f800000 == 0x12000000:                                # AND (immediate)
                    try:
                        m = decode_bitmask((w >> 22) & 1, (w >> 10) & 63, (w >> 16) & 63, w >> 31)
                    except Unimpl:
                        m = 0
                    if m and m & (m + 1) == 0 and m < 0x10000:
                        bounds[rd] = m + 1
                elif w & 0x7f800000 == 0x53000000:                              # UBFM: UBFX, UXTB, UXTH, LSR
                    immr, imms = (w >> 16) & 63, (w >> 10) & 63
                    if imms >= immr and imms - immr < 16:
                        bounds[rd] = 1 << (imms - immr + 1)
        d = defs.get(reg)
        if not d or d[0] != 'table':
            return None
        _, base, tab, esize, signed, shift, mask_bound, idx = d
        # Possible sizes, tried largest first, the first whose entries are all valid taken: the index register's
        # compares shortly before (a chain of CMP / CCMP, main+6d4988; the nearest compare can test another
        # register, main+e6e5e8), the last compare (the index is often a copy of the compared register,
        # sdk+4d7dac), each capped by an AND / UBFX bound. Too few entries lose cases (a guest trap at run time),
        # too many read the next table (rejected when an entry is not code, then a smaller size is tried)
        sizes = {n for a, r, n in cmps if r == idx and before and before[-1] - a <= 0x48}    # sdk+59c7b8: 0x3c
        if last_cmp and before and before[-1] - last_cmp[0] <= 0x30:
            sizes.add(last_cmp[1])
        sizes = {min(n, mask_bound) if mask_bound else n for n in sizes if n <= 0x10000}
        if mask_bound and not sizes:
            sizes = {mask_bound}
        t0, t1 = self.text
        if tab is None:
            if not candidates or not sizes:
                return None
            fs, fe, addrs = candidates
            for bound in sorted(sizes, reverse=True):
                cases = set()
                for c in addrs:
                    got = []
                    for k in range(bound):
                        ea = c + k * esize
                        if not self.m.ro[0] <= ea + esize <= self.m.ro[1]:
                            break
                        t = base + (int.from_bytes(self.m.bytes(ea, esize), 'little', signed=signed) << shift)
                        if t & 3 or not fs <= t < fe:
                            break
                        got.append(t)
                    else:
                        cases.update(got)
                if cases:
                    return sorted(cases)
            return None
        # entries pointing at a zero word are cases that cannot happen (clang's trap for unreachable; sdk+181418)
        sized = None
        for bound in sorted(sizes, reverse=True):   # a known size: every entry must be code nearby
            cases = []
            for k in range(bound):
                ea = tab + k * esize
                if not (self.m.ro[0] <= ea < self.m.ro[1] or t0 <= ea < t1):
                    break
                t = base + (int.from_bytes(self.m.bytes(ea, esize), 'little', signed=signed) << shift)
                if t & 3 or not (t0 <= t < t1) or abs(t - base) >= 0x10000:
                    break
                cases.append(t)
            else:
                if any(self.word(t) for t in cases):
                    sized = cases
                    break
            if len(cases) < bound and not (self.m.ro[0] <= tab + len(cases) * esize < self.m.ro[1] or
                                           t0 <= tab + len(cases) * esize < t1) and any(self.word(t) for t in cases):
                sized = cases                       # the table ends at the segment's end
                break
        # Also read on to the next data object (an address code builds with ADRP + ADD, mod.ro_refs) while the
        # targets stay in or near the function: a size from a compare on another path can be too small
        # (main+3fb1e4: 5 of 6; main+285ab4 has no guard on its path), and reading too far only adds labels (cases
        # are a C switch on the target address), where a missing case is a guest trap at run time
        if inside:
            fs, fe = min(inside), max(inside) + 4
        else:
            fs, fe = base - 0x4000, base + 0x4000
        free = []
        refs = getattr(self, 'ro_refs', ())
        for k in range(1024):
            ea = tab + k * esize
            if not (self.m.ro[0] <= ea < self.m.ro[1] or t0 <= ea < t1) or (k and ea in refs):
                break
            t = base + (int.from_bytes(self.m.bytes(ea, esize), 'little', signed=signed) << shift)
            # a case that starts another function (a tail block with an unwind entry of its own, main+285ab4's
            # second entry) does not end the table: the dynamic path enters functions anyway
            if t & 3 or not (t0 <= t < t1) or not (t in inside or fs <= t < fe or abs(t - base) < 0x400 or
                                                   (t in self.funcs and abs(t - base) < 0x10000) or
                                                   (fe <= t < fe + 0x10 and not self.word(t)) or
                                                   (not inside and abs(t - base) < 0x4000 and self.word(t))):
                break
            free.append(t)
        if sized is None and sizes and not free:
            return None
        cases = list(dict.fromkeys((sized or []) + free))
        if not any(self.word(t) for t in cases):
            return None
        if esize == 1 and shift == 2 and inside:
            cases += self.byte_table_blocks(base, inside)
        return list(dict.fromkeys(cases))

    def byte_table_blocks(self, base, inside):
        """Every possible case of a byte table (base + 4 * 0..255) that starts a block of the function (a branch
        target in it, or after an unconditional branch / return): complete whatever size was guessed, the table's
        address included (main+4ee014's comes from another path). Extra labels cost little."""
        out = []
        for t in range(base, base + 0x400, 4):
            if t not in inside:
                continue
            p = self.word(t - 4)
            if p & 0xfffffc1f in (0xd65f0000, 0xd61f0000) or p & 0xfc000000 == 0x14000000 or \
                    p & 0xffe0001f in (0xd4200000, 0xd4000001) or p == 0:
                out.append(t)
        targets = self._branch_targets_in(inside)
        out += [t for t in range(base, base + 0x400, 4) if t in targets]
        return out

    def _branch_targets_in(self, inside):
        key = min(inside)
        cache = self.__dict__.setdefault('_bt_cache', {})
        if key not in cache:
            cache.clear()                           # one function at a time (recover_switches' order)
            ts = set()
            for a in inside:
                w = self.word(a)
                if w & 0xfc000000 != 0x94000000:
                    ts.update(t - self.base for t in branch_targets(w, self.base + a))
            cache[key] = ts
        return cache[key]

    @staticmethod
    def _table_sum(defs, rn, rm, shift):
        b, e = defs.get(rn), defs.get(rm)
        if b and e and b[0] == 'addr' and e[0] == 'entry':
            return ('table', b[1], e[1], e[2], e[3], shift, e[4], e[5])     # e[1] None: table address unknown
        return None

    @staticmethod
    def _writes(w):
        """General registers an instruction may write (a superset is fine: it only makes the slice give up)."""
        op0 = (w >> 25) & 15
        if op0 & 0b1110 == 0b1000 or op0 & 0b0111 == 0b0101:               # data processing: Rd
            return (w & 31,)
        if op0 & 0b0101 == 0b0100:                                          # loads and stores
            rn, rt, rt2 = (w >> 5) & 31, w & 31, (w >> 10) & 31
            simd = w & 0x04000000
            if w & 0x3b000000 == 0x18000000:                                # LDR (literal)
                return () if simd else (rt,)
            if w & 0x3f000000 == 0x08000000:                                # exclusive / acquire-release
                if w & 0x00400000:                                          # loads: Rt (and Rt2 of a pair)
                    return (rt, rt2) if w & 0x00200000 else (rt,)
                return () if w & 0x00800000 else ((w >> 16) & 31,)          # STXR / STXP: the status register
            if w & 0x3a000000 == 0x28000000:                                # pairs
                wb = (w >> 23) & 3 in (1, 3)                                # post / pre index
                load = w & 0x00400000 and not simd
                return ((rn,) if wb else ()) + ((rt, rt2) if load else ())
            if w & 0x3b000000 == 0x39000000:                                # unsigned offset: no writeback
                wb = False
            elif w & 0x3b200c00 == 0x38200800:                              # register offset
                wb = False
            elif w & 0x3b200000 == 0x38000000:                              # imm9: post / pre index write back
                wb = (w >> 10) & 3 in (1, 3)
            else:                                                           # anything else: assume the worst
                return (rn, rt, rt2)
            load = (w >> 22) & 3 != 0 and not simd                          # LDR*, LDRS* (PRFM: no harm)
            return ((rn,) if wb else ()) + ((rt,) if load else ())
        if w & 0xfc000000 == 0x94000000 or w & 0xfffffc1f == 0xd63f0000:  # BL, BLR: the caller-saved registers
            return tuple(range(19)) + (30,)
        if w & 0xfff00000 == 0xd5300000:                                    # MRS
            return (w & 31,)
        return ()


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
    """All the modules, with imports resolved between them."""

    def __init__(self, names=MODULES, overrides=None):
        self.mods = {n: Mod(n) for n in names}
        self.overrides = overrides or set()       # symbols replaced by host code: calls become h_<symbol>(c)
        self.patches = load_patches(self.mods)    # absolute address -> (was, kind, arg), see load_patches
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


# calls of these also set a host jump buffer in the caller's frame (a64rt.h GUEST_SETJMP); longjmp is an override
SETJMP = {'setjmp', '_setjmp', '__setjmp'}
# the LOAD statements of registers a call leaves as they were (see emit)
KEPT_ACROSS_CALLS = re.compile(r'(X(19|2\d|30) = c->x\[|SP = c->x\[|V(8|9|1[0-5]) = c->v\[)')


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
        if self.mod.plt.get(target - self.mod.base) in SETJMP:
            return 'X30 = 0x%xull; SAVE; GUEST_SETJMP(%s); LOAD;' % (ret_addr, self.env.callee(target))
        name = self.env.callee(target)
        if name.startswith('h_'):                      # host replacements may set any register
            return 'X30 = 0x%xull; SAVE; %s(c); LOAD;' % (ret_addr, name)
        return 'X30 = 0x%xull; SAVEC; %s(c); LOADC;' % (ret_addr, name)

    def call_ind(self, expr, ret_addr):
        self.wx.add(30)
        self.fk = None
        return '{ u64 t = %s; X30 = 0x%xull; SAVEC; call_ind_fast(c, t); LOADC; }' % (expr, ret_addr)

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
            for t in sorted(set(cases)):
                ta = self.mod.base + t
                if ta in self.addrs:
                    self.labels.add(ta)
                    out.append('case 0x%xull: goto L_%x;' % (ta, ta))
            out.append('default: c->pc = JT; SAVE; MUSTTAIL return jump_ind_fast(c); } }')
            return ' '.join(out)
        return '{ c->pc = %s; SAVE; MUSTTAIL return jump_ind_fast(c); }' % expr

    def ret(self, expr):
        return '{ SAVEC; return; }'

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
        # falling through past the end of a chunk into code IDA gave another function (it split this one at a loop
        # head, e.g. sdk 13's SignalLightEvent): an entry there, so that the fall-through becomes a jump
        for i, a in enumerate(offs):
            nxt = a + 4
            if i + 1 < len(offs) and offs[i + 1] == nxt or nxt in own or nxt in mod.funcs:
                continue
            w = mod.word(a)
            if w & 0xfffffc1f in (0xd65f0000, 0xd61f0000) or w & 0xfc000000 == 0x14000000 \
                    or w & 0xffe0001f == 0xd4200000 or w == 0 or w == 0x94000002:
                continue
            o = mod.owner.get(nxt)
            if o is not None and o != off:
                sec[o].add(nxt)
    # jump table cases in another function (IDA made the cases a function of their own): entered by jump_ind
    for br, cases in mod.switches.items():
        o = mod.owner.get(br)
        for t in cases:
            ot = mod.owner.get(t)
            if ot is not None and ot != o and t not in mod.funcs:
                sec[ot].add(t)
    # code whose address is taken (relocation targets, exports, init_array, ADR / ADRP+ADD) and that IDA put
    # inside another function: it is entered through call_ind, so it needs an entry too
    for t in set(mod.m.code_seeds()) | mod.addr_refs:
        if t in mod.funcs or not (mod.text[0] <= t < mod.text[1]):
            continue
        o = mod.owner.get(t)
        if o is not None:
            sec[o].add(t)
    mod.sec_entries = {t for v in sec.values() for t in v}
    return {o: sorted(v) for o, v in sec.items()}, orphans


def code_address_refs(mod):
    """Text addresses computed by ADR, or by ADRP followed by ADD to the same register. The read-only addresses
    computed by ADRP + ADD go to mod.ro_refs: where data objects start (jump_table ends a table at the next one)."""
    def sx(v, b):
        return v - (1 << b) if v >> (b - 1) & 1 else v
    out = set()
    ro = set()
    r0, r1 = mod.m.ro
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
                    elif r0 <= t < r1:
                        ro.add(t)
                if w2 & 0xfc000000 in (0x14000000, 0x94000000) or w2 & 0xfffffc1f == 0xd65f0000 or \
                        (w2 & 0x9f000000 == 0x90000000 and w2 & 31 == rd):
                    break             # B, BL, RET or a new ADRP to rd end the search (extra hits only add entries)
            continue
        else:
            continue
        if t0 <= t < t1 and not t & 3:
            out.add(t)
    mod.ro_refs = ro
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
            patch = env.prog.patches.get(pc)
            stmt = lift_patched(fn, w, pc, patch) if patch else lift_insn(fn, w, pc)
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


def lift_patched(fn, w, pc, patch):
    """The instruction at pc with a patch from patches.txt (see load_patches)."""
    was, kind, arg = patch
    if w != was:
        raise SystemExit('patches.txt: %x holds %08x, not %08x (another version of the game?)' % (pc, w, was))
    if kind == 'if':
        name, new = arg
        fk = fn.fk
        s_new = lift_insn(fn, new, pc)
        fn.fk = fk
        s_old = lift_insn(fn, w, pc)
        fn.fk = None
        return '{ extern volatile u8 nx_flag_%s; if (nx_flag_%s) { %s } else { %s } }' % (name, name, s_new, s_old)
    call = '{ extern void h_%s(Ctx *); SAVE; h_%s(c); LOAD; }' % (arg, arg)
    fn.fk = None
    return call if kind == 'call' else lift_insn(fn, w, pc) + ' ' + call


def env_is_entry(env, addr):
    off = addr - env.mod.base
    return off in env.mod.funcs or off in getattr(env.mod, 'sec_entries', ())


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
    # Calls (BL, BLR) and returns follow the AAPCS64: NZCV is not passed or kept, and the callee keeps x19-x30, sp
    # and the low halves of v8-v15 (the caller's own values are kept in its locals), so SAVEC leaves the flags out
    # and LOADC reloads only what a call may change. Every register still goes to the Ctx before a call, so a
    # callee that spills one (and anything reading those spills) sees the current value.
    savec, loadc = list(save), [s for s in load if not KEPT_ACROSS_CALLS.match(s)]
    if fn.flags:
        save.append('c->nf = NF; c->zf = ZF; c->cf = CF; c->vf = VF;')
        load.append('NF = c->nf; ZF = c->zf; CF = c->cf; VF = c->vf;')
    out.append('#define SAVE do { %s } while (0)' % ' '.join(save))
    out.append('#define LOAD do { %s } while (0)' % ' '.join(load))
    out.append('#define SAVEC do { %s } while (0)' % ' '.join(savec))
    out.append('#define LOADC do { %s } while (0)' % ' '.join(loadc))
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
    out.append('#undef SAVEC')
    out.append('#undef LOADC')
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


# ----------------------------------------------------------------------------- parallel lifting
# Functions lift independently (Env only collects names), so workers lift chunks of them in their own Program
# (~2.6 GB each for Violet) and the files are put together here in the same order: the output is the same.
LIFT_CHUNK = 1000
_worker_prog = None


def lift_procs():
    if os.environ.get('NX_LIFT_PROCS'):
        return max(1, int(os.environ['NX_LIFT_PROCS']))
    avail_gb = 8.0
    if os.name == 'nt':
        class MemStatus(ctypes.Structure):
            _fields_ = [('len', ctypes.c_uint32), ('load', ctypes.c_uint32), ('total', ctypes.c_uint64),
                        ('avail', ctypes.c_uint64), ('pf_total', ctypes.c_uint64), ('pf_avail', ctypes.c_uint64),
                        ('virt_total', ctypes.c_uint64), ('virt_avail', ctypes.c_uint64), ('ext', ctypes.c_uint64)]
        ms = MemStatus(len=ctypes.sizeof(MemStatus))
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms)):
            avail_gb = ms.avail / 2**30
    return max(1, min(8, os.cpu_count() or 1, int((avail_gb - 4) / 3)))   # 4 GB: this process and its results


def _lift_worker_init(overrides):
    global _worker_prog
    _worker_prog = Program(overrides=overrides)


def _lift_chunk(job, prog=None):
    """Lift functions of one module: their sources, and what their Env and statistics collected."""
    n, offs, sec = job
    prog = prog or _worker_prog
    if not hasattr(prog.mods[n], 'sec_entries'):
        secondary_entries(prog.mods[n])     # sets mod.sec_entries, which env_is_entry reads
    env = Env(prog, prog.mods[n])
    stats = {'ok': 0, 'bad': collections.Counter(), 'example': {}}
    srcs = [lift_function(env, off, sec.get(off, ()), stats) for off in offs]
    return srcs, env.extra, env.ext_callees, env.unresolved, stats


def cmd_lift(outdir, only=None, per_file=40000):
    overrides = load_overrides()
    prog = Program(overrides=overrides)
    procs = 1 if only else lift_procs()
    pool = None
    if procs > 1:
        print('lifting in %d processes' % procs)
        pool = concurrent.futures.ProcessPoolExecutor(procs, initializer=_lift_worker_init, initargs=(overrides,))
    os.makedirs(outdir, exist_ok=True)
    stats = {'ok': 0, 'bad': collections.Counter(), 'example': {}}
    all_entries = {}
    all_files = []
    ext = set()
    unresolved = {}                          # u_<mangled> -> symbol
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

        jobs = [(n, todo[i:i + LIFT_CHUNK], {o: sec[o] for o in todo[i:i + LIFT_CHUNK] if o in sec})
                for i in range(0, len(todo), LIFT_CHUNK)]
        results = pool.map(_lift_chunk, jobs) if pool else (_lift_chunk(j, prog) for j in jobs)
        for job, (srcs, extra, callees, unres, st) in zip(jobs, results):
            env.extra |= extra
            env.ext_callees |= callees
            env.unresolved.update(unres)
            stats['ok'] += st['ok']
            stats['bad'].update(st['bad'])
            for k, v in st['example'].items():
                stats['example'].setdefault(k, v)
            for off, src in zip(job[1], srcs):
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
        unresolved.update(('u_' + mangle(sym), sym) for sym in env.unresolved)
        if env.unresolved:
            print('%s: %d unresolved imports, e.g. %s' % (n, len(env.unresolved), ', '.join(sorted(env.unresolved)[:8])))
        print('%s: %d files, %d functions' % (n, len(files), len(done)))
    if pool:
        pool.shutdown()
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
        # weak imports nothing defines: rtld leaves them 0, so the guest only calls them behind a null check; a call
        # that does happen would jump to 0 on the console, here it stops with the name
        for u, sym in sorted(unresolved.items()):
            f.write('void %s(Ctx *c) { guest_trap(c, "call to the unresolved weak import %s", c->x[30]); }\n' % (u, sym))
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


def load_patches(mods):
    """The game's instruction patches (patches.txt next to game.json), one per line:
        <module>+<offset> was <word> if <flag> <word>   both lifted: the second while nx_flag_<flag> (a u8 the game's
                                                        host code defines) is nonzero, else the original
        <module>+<offset> was <word> call <name>        h_<name>(c) runs in place of the instruction
        <module>+<offset> was <word> after <name>       h_<name>(c) runs after it
    `was` is the original instruction: a mismatch (another build of the game) stops the lift. The replacement words
    and patched instructions must not branch."""
    out = {}
    path = os.path.join(gamecfg.DIR, 'patches.txt')
    if not os.path.exists(path):
        return out
    for ln in open(path):
        t = ln.split('#')[0].split()
        if not t:
            continue
        m, off = t[0].split('+')
        if m not in mods or t[1] != 'was' or t[3] not in ('if', 'call', 'after'):
            raise SystemExit('patches.txt: bad line: ' + ln.strip())
        addr = mods[m].base + int(off, 16)
        was = int(t[2], 16)
        arg = (t[4], int(t[5], 16)) if t[3] == 'if' else t[4]
        for wd in (was, arg[1]) if t[3] == 'if' else (was,):
            if branch_targets(wd, addr) or (wd & 0x7c000000) == 0x14000000 or (wd & 0xfe000000) == 0xd6000000:
                raise SystemExit('patches.txt: %x: %08x branches' % (addr, wd))
        out[addr] = (was, t[3], arg)
    return out


def load_overrides():
    """The shared host replacements (overrides.txt here) and the game's own (overrides.txt next to game.json)."""
    out = set()
    for path in (os.path.join(HERE, 'overrides.txt'), gamecfg.OVERRIDES):
        if os.path.exists(path):
            out |= {ln.split('#')[0].strip() for ln in open(path) if ln.split('#')[0].strip()}
    return out


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
