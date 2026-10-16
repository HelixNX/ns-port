"""Disassemble the function containing a guest offset, naming calls through PLT stubs and exports.

Usage: python a64dis.py <module> <offset hex> [<offset hex>...]     (module: main, sdk, subsdk0)
       python a64dis.py <module> <start hex>-<end hex>             (a raw range)
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lifter'))
from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs  # noqa: E402
import a64lift  # noqa: E402


def names(mod):
    n = {off: 'plt:' + s for off, s in mod.plt.items()}
    for s, v in mod.exports.items():
        n.setdefault(v, s)
    return n


def dis(mod, lo, hi, nm):
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cs.skipdata = True
    code = mod.m.bytes(lo, hi - lo)
    for i in cs.disasm(code, lo):
        note = ''
        if i.mnemonic in ('bl', 'b') or i.mnemonic.startswith('b.') or i.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            t = int(i.op_str.split('#')[-1], 16)
            if t in nm:
                note = '  ; ' + nm[t]
            elif i.mnemonic == 'bl' and t in mod.owner and mod.owner[t] != t:
                note = '  ; inside %x' % mod.owner[t]
        print('%8x  %-8s %s%s' % (i.address, i.mnemonic, i.op_str, note))


def main():
    mod = a64lift.Mod(sys.argv[1])
    nm = names(mod)
    for arg in sys.argv[2:]:
        if '-' in arg:
            lo, hi = (int(x, 16) for x in arg.split('-'))
        else:
            a = int(arg, 16)
            f = mod.owner.get(a & ~3)
            if f is None:
                print('%x: not in a function' % a)
                continue
            addrs = mod.funcs[f]
            lo, hi = f, max(addrs) + 4
            print('function %x (%s) .. %x' % (f, nm.get(f, ''), hi))
        dis(mod, lo, hi, nm)
        print()


if __name__ == '__main__':
    main()
