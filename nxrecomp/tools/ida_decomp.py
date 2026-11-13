"""Whole-module decompile with IDA (idat batch, Hex-Rays). Two steps, both run by ida_decomp.ps1:

    analyze [functions.json]     auto-analysis of a fresh module; functions.json (a <module>_ida.json export) adds
                                 the function entries the lifter knows and IDA did not find. Saves <module>.elf.i64.
    decomp <out dir> <i> <n>     decompile shard i of n (blocks of 256 functions, dealt round robin) of an analysed
                                 database to <out dir>/<module>_<i>.c; progress in <module>_<i>.log. Run on a copy
                                 of the .i64: the copy is thrown away.

Each function is preceded by a line "// ==== <ea hex> <name>"; a failure is "// ==== <ea> <name> FAILED: <why>".
"""
import json
import os
import time

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_loader
import ida_nalt
import ida_pro
import idautils
import idc

BLOCK = 256


def analyze(seeds):
    t0 = time.time()
    ida_auto.auto_wait()
    print('analysis done in %.0fs, %d functions' % (time.time() - t0, ida_funcs.get_func_qty()), flush=True)
    if seeds:
        made = 0
        for f in json.load(open(seeds))['functions']:
            ea = f['ea']
            if not ida_funcs.get_func(ea) and not ida_bytes.is_data(ida_bytes.get_flags(ea)):
                made += bool(ida_funcs.add_func(ea))
        ida_auto.auto_wait()
        print('seeds: %d functions created, %d functions, %.0fs' % (made, ida_funcs.get_func_qty(), time.time() - t0),
              flush=True)


def decomp(out_dir, shard, shards):
    module = os.path.splitext(ida_nalt.get_root_filename())[0]
    base = os.path.join(out_dir, '%s_%d' % (module, shard))
    log = open(base + '.log', 'w')

    def say(s):
        log.write(s + '\n')
        log.flush()

    ida_auto.auto_wait()
    if not ida_hexrays.init_hexrays_plugin():
        say('no decompiler')
        return 1
    funcs = [ea for k, ea in enumerate(idautils.Functions()) if (k // BLOCK) % shards == shard]
    say('%d functions' % len(funcs))
    t0, failed = time.time(), 0
    with open(base + '.c', 'w', encoding='utf-8', errors='replace') as out:
        for k, ea in enumerate(funcs):
            name = idc.get_func_name(ea)
            hf = ida_hexrays.hexrays_failure_t()
            try:
                cf = ida_hexrays.decompile(ea, hf, ida_hexrays.DECOMP_NO_WAIT | ida_hexrays.DECOMP_NO_CACHE)
                text = str(cf) if cf else None
            except Exception as e:  # DecompilationFailure and anything a broken function throws
                text, hf = None, e
            if text is None:
                failed += 1
                why = hf.desc() if hasattr(hf, 'desc') else str(hf)
                out.write('// ==== %x %s FAILED: %s\n\n' % (ea, name, why.replace('\n', ' ')))
            else:
                out.write('// ==== %x %s\n%s\n\n' % (ea, name, text))
            if k % 500 == 499:
                ida_hexrays.clear_cached_cfuncs()
                out.flush()
                say('%d / %d, %d failed, %.0fs' % (k + 1, len(funcs), failed, time.time() - t0))
    say('done %d, %d failed, %.0fs' % (len(funcs), failed, time.time() - t0))
    return 0


def main():
    a = idc.ARGV[1:]
    code = 0
    try:
        if a[0] == 'analyze':
            analyze(a[1] if len(a) > 1 else None)
        else:
            code = decomp(a[1], int(a[2]), int(a[3]))
            ida_loader.set_database_flag(ida_loader.DBFL_KILL)   # a shard's copy is not kept
    except Exception:
        import traceback
        traceback.print_exc()
        code = 1
    ida_pro.qexit(code)


main()
