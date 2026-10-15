"""Run IDA (idalib, headless) over a converted module and export what the lifter needs:
function entries and their chunks, and the data items (literal pools, jump tables) that sit inside .text.

Usage (with the ida-mcp venv's Python, IDADIR set):
    python ida_export.py <module.elf> <out.json>
The IDA database is written next to the ELF and reused on the next run.
"""
import json
import os
import sys
import time

import idapro


def main(elf, out):
    t0 = time.time()
    elf = os.path.abspath(elf)
    db = elf + '.i64'
    if idapro.open_database(db if os.path.exists(db) else elf, run_auto_analysis=True):
        raise SystemExit('IDA could not open ' + elf)
    import ida_auto
    import ida_bytes
    import ida_funcs
    import ida_idaapi
    import ida_segment
    import idautils
    ida_auto.auto_wait()
    print('analysis done in %.0fs' % (time.time() - t0), flush=True)
    seeds = os.environ.get('IDA_SEEDS')
    if seeds:
        seed_functions(json.load(open(seeds)))
        print('seeding done in %.0fs' % (time.time() - t0), flush=True)
    try:
        export(elf, out, t0)
    finally:
        idapro.close_database(save=True)


def seed_functions(seeds):
    """Create functions at code addresses IDA did not reach (virtual tables, callbacks), then at the start of
    every remaining stretch of unexplored bytes in .text, until nothing changes."""
    import ida_auto
    import ida_bytes
    import ida_funcs
    import ida_idaapi
    import ida_segment
    text = ida_segment.get_segm_by_name('.text')
    made = 0
    for ea in seeds:
        if text.start_ea <= ea < text.end_ea and ea % 4 == 0 and not ida_funcs.get_func(ea) \
                and not ida_bytes.is_data(ida_bytes.get_flags(ea)):
            made += bool(ida_funcs.add_func(ea))
    ida_auto.auto_wait()
    print('  seeds: %d functions created' % made, flush=True)
    for rnd in range(8):
        made, ea = 0, text.start_ea
        while ea < text.end_ea:
            fl = ida_bytes.get_flags(ea)
            if ida_funcs.get_func(ea) or ida_bytes.is_data(fl):
                ea += max(ida_bytes.get_item_size(ea), 4) if ida_bytes.is_data(fl) else 4
                f = ida_funcs.get_func(ea - 4)
                if f and f.end_ea > ea:
                    ea = f.end_ea
                continue
            if ida_bytes.get_dword(ea) != 0 and (ida_bytes.is_code(fl) or ida_bytes.is_unknown(fl)):
                if ida_funcs.add_func(ea):
                    made += 1
                    f = ida_funcs.get_func(ea)
                    ea = f.end_ea if f else ea + 4
                    continue
            ea += 4
        ida_auto.auto_wait()
        print('  gap sweep %d: %d functions created' % (rnd, made), flush=True)
        if not made:
            break


def export(elf, out, t0):
    import ida_bytes
    import ida_funcs
    import ida_idaapi
    import ida_segment
    import idautils

    text = ida_segment.get_segm_by_name('.text')
    funcs = []
    for ea in idautils.Functions(text.start_ea, text.end_ea):
        f = ida_funcs.get_func(ea)
        chunks = [[s, e] for s, e in idautils.Chunks(ea)]
        funcs.append({'ea': ea, 'end': f.end_ea, 'chunks': chunks,
                      'noret': bool(f.flags & ida_funcs.FUNC_NORET), 'thunk': bool(f.flags & ida_funcs.FUNC_THUNK)})
    # data items inside .text, merged into ranges
    data, ea = [], text.start_ea
    while ea < text.end_ea:
        fl = ida_bytes.get_flags(ea)
        if ida_bytes.is_data(fl):
            size = max(ida_bytes.get_item_size(ea), 1)
            if data and data[-1][1] == ea:
                data[-1][1] = ea + size
            else:
                data.append([ea, ea + size])
            ea += size
        else:
            nxt = ida_bytes.next_head(ea, text.end_ea)
            ea = nxt if nxt != ida_idaapi.BADADDR and nxt > ea else text.end_ea
    code_bytes = sum(sum(e - s for s, e in f['chunks']) for f in funcs)
    json.dump({'module': os.path.basename(elf), 'text': [text.start_ea, text.end_ea],
               'functions': funcs, 'text_data': data}, open(out, 'w'))
    print('functions %d  code bytes in functions %x of %x  data ranges in .text %d (%x bytes)  total %.0fs' % (
        len(funcs), code_bytes, text.end_ea - text.start_ea, len(data), sum(e - s for s, e in data), time.time() - t0))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
