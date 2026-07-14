#!/usr/bin/env python3
"""Temporal realism audit of the BLE persistent-device generator.

Feed it the output of `synth_dump --devices <seed> <n> <ticks> <tick_ms>`:
    synth_dump --devices 1 24 8000 1000 > run.txt
    python rotation_audit.py run.txt
Exit non-zero on any FAIL. Optional: `--profile profile.json` also prints the
Jensen-Shannon divergence of the synthetic vs real per-address presence-duration
histogram (the passively-observable projection of rotation + lifetime).
"""
import sys, argparse, math
from collections import defaultdict, Counter

RPA_LO, RPA_HI = 600000, 1200000            # 10-20 min band (ms)
TRANSIENT_CAP, RESIDENT_CAP = 720000, 5400000
PRESENCE_BINS = [0, 60000, 300000, 900000, 1800000, 3600000, 7200000, 10**12]  # <1,5,15,30,60,120,>120 min

def parse(fh):
    rows = []
    for ln in fh:
        p = ln.split()
        if len(p) == 9 and p[0] == "D":
            rows.append((int(p[1]), int(p[2]), p[3], p[4], p[5], p[6], int(p[7]), int(p[8])))
    return rows

def segments(rows):
    by_slot = defaultdict(list)
    for r in sorted(rows, key=lambda r: (r[1], r[0])):
        by_slot[r[1]].append(r)
    segs = []
    for evs in by_slot.values():
        cur = None
        for e in evs:
            if e[5] == "born":
                if cur: segs.append(cur)
                cur = [e]
            elif cur:
                cur.append(e)
        if cur: segs.append(cur)
    return segs

def jsd(p, q):
    s = lambda v: [x / (sum(v) or 1) for x in v]
    p, q = s(p), s(q); m = [(a + b) / 2 for a, b in zip(p, q)]
    kl = lambda a, b: sum(x * math.log2(x / y) for x, y in zip(a, b) if x > 0 and y > 0)
    return (kl(p, m) + kl(q, m)) / 2

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile", nargs="?", help="devices dump (default: stdin)")
    ap.add_argument("--profile", help="profile.json with presence_ms_bins for real-vs-synth JSD")
    a = ap.parse_args()
    rows = parse(open(a.infile) if a.infile else sys.stdin)
    if not rows:
        print("FAIL: no device events parsed"); sys.exit(1)
    segs = segments(rows)
    fails = []

    # 1. static never rotates; RPA gaps in band
    for s in segs:
        at = s[0][3]
        ts = [e[0] for e in s if e[5] in ("born", "rotate")]
        gaps = [b - a for a, b in zip(ts, ts[1:])]
        if at == "static" and any(e[5] == "rotate" for e in s):
            fails.append("static device rotated")
        if at == "rpa":
            for g in gaps:
                if not (RPA_LO - 1000 <= g <= RPA_HI + 1000):
                    fails.append(f"RPA rotation out of band: {g} ms")

    # 2. lifetime cohort bounded by role
    by_slot = defaultdict(list)
    for r in rows:
        if r[5] == "born": by_slot[r[1]].append(r)
    for slot, births in by_slot.items():
        births = sorted(births, key=lambda x: x[0])
        for r_a, r_b in zip(births, births[1:]):
            if r_a[4] == "persistent": continue         # infrastructure: no lifetime cap by design
            cap = TRANSIENT_CAP if r_a[4] == "transient" else RESIDENT_CAP
            if r_b[0] - r_a[0] > cap + 1000:
                fails.append(f"{r_a[4]} lifetime over cap: {r_b[0] - r_a[0]} ms")

    # 3. rotation independence (no synchronized volley)
    live = max(e[1] for e in rows) + 1
    per_tick = Counter(e[0] for e in rows if e[5] == "rotate")
    if per_tick and max(per_tick.values()) >= max(2, live // 2):
        fails.append("synchronized rotation volley")

    # 4. no address resurrection
    seen = set()
    for _, _, addr, *_ in rows:
        if addr in seen: fails.append("address reused"); break
        seen.add(addr)

    for label, ok in [("rotation-cadence", not any("RPA rotation" in f or "static" in f for f in fails)),
                      ("lifetime-cohort", not any("lifetime" in f for f in fails)),
                      ("rotation-independence", "synchronized rotation volley" not in fails),
                      ("no-resurrection", "address reused" not in fails)]:
        print(f"{'PASS' if ok else 'FAIL'}  {label}")

    if a.profile:
        import json
        prof = json.load(open(a.profile)).get("presence_ms_bins")
        if prof:
            # Per-ADDRESS presence = how long each address stays observable = the gap until the
            # SAME SLOT's next address replaces it (last address in a slot -> to sim end). NOT the
            # within-segment span (born->last-rotate), which scores a non-rotating static address
            # as 0 and badly under-counts presence -- the whole point capture_profile measures.
            tmax = max(r[0] for r in rows)
            slot_ts = defaultdict(list)
            for r in rows: slot_ts[r[1]].append(r[0])
            synth = [0] * (len(PRESENCE_BINS) - 1)
            for ts in slot_ts.values():
                ts.sort()
                for i in range(len(ts)):
                    d = (ts[i+1] if i+1 < len(ts) else tmax) - ts[i]
                    for k in range(len(PRESENCE_BINS) - 1):
                        if PRESENCE_BINS[k] <= d < PRESENCE_BINS[k+1]: synth[k] += 1; break
            print(f"presence-duration JSD vs real: {jsd(synth, prof):.4f}")

    if fails:
        print("FAILS:", "; ".join(sorted(set(fails)))); sys.exit(1)
    print("ALL TEMPORAL CHECKS PASS")

if __name__ == "__main__":
    main()
