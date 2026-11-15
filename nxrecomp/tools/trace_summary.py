"""Per-frame summary of an NX_TRACE timeline (runtime/trace.c): py -3.12 trace_summary.py <trace-N.json> [threads...]

For each frame (present end to present end): its length, the GPU's busy time and its idle gaps, and for each busy
guest thread (or the ones named) the time running (and of it on a host CPU), waiting for a core, blocked, sleeping, and in the runtime's
work. Then, over all frames: where those threads block (the guest chain from its first frame outside the SDK), how
often, who ended the waits and how long the host took from that to the thread running again.
"""
import json
import sys
from collections import defaultdict


def load(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    names, frames, spans, gpu = {}, [], [], []
    for e in d["traceEvents"]:
        ph = e.get("ph")
        if ph == "M" and e["name"] == "thread_name":
            names[(e["pid"], e["tid"])] = e["args"]["name"]
        elif ph == "i" and e["name"].startswith("frame "):
            frames.append((e["ts"], int(e["name"][6:])))
        elif ph == "X":
            if e["pid"] == 3:
                gpu.append((e["ts"], e["ts"] + e["dur"]))
            else:
                spans.append((e["pid"], e["tid"], e["name"], e["ts"], e["ts"] + e["dur"], e.get("args", {})))
    frames.sort()
    gpu.sort()
    return d.get("otherData", {}), names, frames, spans, gpu


def clip(a0, a1, b0, b1):
    return max(0.0, min(a1, b1) - max(a0, b0))


def union(iv, f0, f1):
    """busy time of intervals iv within [f0, f1), and the idle gaps over 50 us"""
    busy, gaps, at = 0.0, [], f0
    for s, e in iv:
        if e <= f0 or s >= f1:
            continue
        s, e = max(s, f0), min(e, f1)
        if s > at:
            if s - at > 50:
                gaps.append((at, s))
        if e > at:
            busy += e - max(s, at)
            at = e
    if f1 - at > 50:
        gaps.append((at, f1))
    return busy, gaps


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    other, names, frames, spans, gpu = load(sys.argv[1])
    want = sys.argv[2:]
    by_track = defaultdict(list)
    for sp in spans:
        by_track[(sp[0], sp[1])].append(sp)
    for v in by_track.values():
        v.sort(key=lambda s: s[3])

    def tname(pid, tid):
        return names.get((pid, tid), f"{pid}/{tid}")

    # guest threads: state track 2*id, runtime track 2*id+1
    guests = sorted({tid // 2 for (pid, tid) in by_track if pid == 1})
    if want:
        show = [g for g in guests if tname(1, 2 * g) in want]
    else:
        total = {g: sum(s[4] - s[3] for s in by_track.get((1, 2 * g), []) if s[2] == "running") for g in guests}
        show = [g for g in sorted(guests, key=lambda g: -total[g]) if total[g] > 0][:6]

    print(f"{sys.argv[1]}: frames {frames[0][1]}-{frames[-1][1]}, {len(spans)} spans, {len(gpu)} GPU submissions, "
          f"dropped {other.get('dropped', 0)}")
    hdr = "frame   ms  | GPU busy idle (gaps)  | " + " | ".join(f"{tname(1, 2 * g)[:12]:>12} run/cpu/rdy/blk/rt"
                                                         for g in show)
    print(hdr)
    sums = defaultdict(float)
    for (f0, fn0), (f1, fn1) in zip(frames, frames[1:]):
        busy, gaps = union(gpu, f0, f1)
        cells = []
        for g in show:
            st = by_track.get((1, 2 * g), [])
            rt = by_track.get((1, 2 * g + 1), [])
            run = sum(clip(s[3], s[4], f0, f1) for s in st if s[2] == "running")
            # the part of it the host thread really ran (runtime/kernel.c hold_trace; spans without it count whole)
            cpu = sum(clip(s[3], s[4], f0, f1) * min(100.0, s[5].get("host cpu %", 100.0)) / 100 for s in st
                      if s[2] == "running")
            rdy = sum(clip(s[3], s[4], f0, f1) for s in st if s[2] == "waiting for a core")
            blk = sum(clip(s[3], s[4], f0, f1) for s in st if s[2].startswith("blocked") or s[2] == "sleep")
            # runtime spans nest (vkQueueSubmit inside translate + submit): count the outer ones
            r = sum(clip(s[3], s[4], f0, f1) for s in rt if s[2] != "vkQueueSubmit")
            sums[(g, "run")] += run
            sums[(g, "cpu")] += cpu
            sums[(g, "rdy")] += rdy
            sums[(g, "blk")] += blk
            sums[(g, "rt")] += r
            cells.append(f"{run / 1000:4.2f} {cpu / 1000:4.2f} {rdy / 1000:4.2f} {blk / 1000:4.2f} {r / 1000:4.2f}".rjust(30))
        sums["frame"] += f1 - f0
        sums["busy"] += busy
        print(f"{fn1:6d} {(f1 - f0) / 1000:5.2f} | {busy / 1000:5.2f} {(f1 - f0 - busy) / 1000:4.2f} ({len(gaps):2d})  | "
              + " | ".join(cells))
    nf = max(1, len(frames) - 1)
    print("mean   " + f"{sums['frame'] / nf / 1000:5.2f} | {sums['busy'] / nf / 1000:5.2f} "
          f"{(sums['frame'] - sums['busy']) / nf / 1000:4.2f}       | "
          + " | ".join(f"{sums[(g, 'run')] / nf / 1000:4.2f} {sums[(g, 'cpu')] / nf / 1000:4.2f} "
                       f"{sums[(g, 'rdy')] / nf / 1000:4.2f} {sums[(g, 'blk')] / nf / 1000:4.2f} "
                       f"{sums[(g, 'rt')] / nf / 1000:4.2f}".rjust(30) for g in show))

    t0, t1 = frames[0][0], frames[-1][0]
    print("\nruntime work per frame (all threads):")
    rt = defaultdict(float)
    for (pid, tid), v in by_track.items():
        for s in v:
            if pid == 2 or tid % 2:
                rt[(tname(pid, tid), s[2])] += clip(s[3], s[4], t0, t1)
    for (n, k), ms in sorted(rt.items(), key=lambda x: -x[1])[:12]:
        print(f"  {ms / nf / 1000:6.3f} ms  {n}: {k}")

    def site(args):
        """a block's site: the chain from its first frame outside the SDK's wrappers (main+..., at most 3)"""
        ch = args.get("chain", "").split(" < ")
        i = next((k for k, a in enumerate(ch) if not a.startswith("sdk+")), len(ch) - 1)
        return " < ".join(ch[i:i + 3])

    print("\nblocked, by site (ms per frame, waits per frame, who ended the waits, mean us from that to running):")
    for g in show:
        acc = defaultdict(lambda: [0.0, 0, defaultdict(int), 0.0, 0])
        for s in by_track.get((1, 2 * g), []):
            if s[2].startswith("blocked") or s[2] == "sleep":
                a = acc[(s[2].replace("blocked: ", ""), site(s[5]))]
                a[0] += clip(s[3], s[4], t0, t1)
                a[1] += 1
                a[2][s[5].get("woken by", "-")] += 1
                if "wake us" in s[5]:
                    a[3] += s[5]["wake us"]
                    a[4] += 1
        print(f"  {tname(1, 2 * g)}:")
        for (kind, at), (ms, n, who, wus, nw) in sorted(acc.items(), key=lambda x: -x[1][0])[:6]:
            whos = ", ".join(f"{w} {c}" for w, c in sorted(who.items(), key=lambda x: -x[1])[:3])
            print(f"    {ms / nf / 1000:6.3f} {n / nf:5.1f}/f  {kind:8} {at}  [{whos}; {wus / max(nw, 1):.1f} us]")


if __name__ == "__main__":
    main()
