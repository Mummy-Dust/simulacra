#!/usr/bin/env python3
"""Score the probe-behavioral-realism metrics from a C5 sniffer serial log.

The sniffer (main/sniff.c, SIMULACRA_SNIFF=1, SNIFF_LOG_FRAMES=1) prints one line per
probe request it hears:

    W (12199) sniff: pr sa=aa:bb:cc:dd:ee:ff seq=1234 rssi=-52

Feed a saved log (or pipe it) here. Three PASS/FAIL checks, matching the spec's validation bar:

  1. seq-independence  -- FAIL if any run of >=3 frames (in time order) increments seq by exactly 1
                          while the source MAC changes  (= one shared hardware counter, the bug).
  2. decorrelation     -- FAIL if any tight window shows a big simultaneous volley of distinct MACs
                          (= lockstep). Reports the max concurrent distinct MACs.
  3. constellation     -- reports distinct MACs + first-third / last-third overlap; FAIL if overlap
                          is high (the set never turned over).  A short log under-reports churn.

Optional --rssi-min / --rssi-max isolate the co-located decoys (e.g. --rssi-min -60 --rssi-max -45)
from strong nearby real devices; by default every probe is analyzed (metric 1 is robust either way,
since a real device's independent counter cannot forge a cross-MAC consecutive run).

    python sniff_analyze.py <logfile>            # or:  ... | python sniff_analyze.py -
"""
import sys, re, argparse
from collections import defaultdict, Counter

LINE = re.compile(r"\((\d+)\)\s+sniff:\s+pr sa=([0-9a-fA-F:]{17})\s+seq=(\d+)\s+rssi=(-?\d+)")


def load(fh, rssi_min, rssi_max):
    frames = []   # (t_ms, mac, seq, rssi) in file order
    for ln in fh:
        m = LINE.search(ln)
        if not m:
            continue
        t, mac, seq, rssi = int(m.group(1)), m.group(2).lower(), int(m.group(3)), int(m.group(4))
        if rssi_min is not None and rssi < rssi_min:
            continue
        if rssi_max is not None and rssi > rssi_max:
            continue
        frames.append((t, mac, seq, rssi))
    return frames


def check_seq_independence(frames):
    # scan time-ordered frames for a >=3-long run of +1 seq steps across changing MACs
    run, worst = 1, 0
    bad = []
    for (t0, m0, s0, _), (t1, m1, s1, _) in zip(frames, frames[1:]):
        if s1 == (s0 + 1) & 0x0FFF and m1 != m0:
            run += 1
            if run >= 3:
                bad.append((m0, s0, m1, s1))
        else:
            run = 1
        worst = max(worst, run)
    ok = not bad
    print(f"[1] seq-independence : {'PASS' if ok else 'FAIL'}  "
          f"(longest cross-MAC +1 run = {worst}; want < 3)")
    for m0, s0, m1, s1 in bad[:4]:
        print(f"      shared-counter signature: {m0} seq={s0} -> {m1} seq={s1}")
    return ok


def check_decorrelation(frames, window_ms=25, volley=6):
    # max distinct MACs within any window_ms slice
    worst, worst_t = 0, 0
    i = 0
    for j in range(len(frames)):
        t = frames[j][0]
        while frames[i][0] < t - window_ms:
            i += 1
        macs = {frames[k][1] for k in range(i, j + 1)}
        if len(macs) > worst:
            worst, worst_t = len(macs), t
    ok = worst < volley
    print(f"[2] decorrelation    : {'PASS' if ok else 'FAIL'}  "
          f"(max {worst} distinct MACs within {window_ms}ms @t={worst_t}; want < {volley})")
    return ok


def check_constellation(frames, overlap_max=0.6):
    macs = [f[1] for f in frames]
    distinct = set(macs)
    n = len(frames)
    if n < 6:
        print(f"[3] constellation    : distinct MACs={len(distinct)} (log too short to judge churn)")
        return True
    a = set(macs[: n // 3])
    b = set(macs[2 * n // 3:])
    jac = len(a & b) / len(a | b) if (a | b) else 1.0
    ok = jac <= overlap_max
    print(f"[3] constellation    : {'PASS' if ok else 'FAIL'}  "
          f"(distinct MACs={len(distinct)}, first/last-third overlap={jac:.2f}; want <= {overlap_max})")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile", help="sniffer serial log, or - for stdin")
    ap.add_argument("--rssi-min", type=int, default=None)
    ap.add_argument("--rssi-max", type=int, default=None)
    a = ap.parse_args()
    fh = sys.stdin if a.logfile == "-" else open(a.logfile, encoding="utf-8", errors="ignore")
    frames = load(fh, a.rssi_min, a.rssi_max)
    if not frames:
        print("no probe frames parsed (check the log / rssi filter)")
        sys.exit(2)
    print(f"parsed {len(frames)} probe frames, {len({f[1] for f in frames})} distinct MACs")
    if a.rssi_min is not None or a.rssi_max is not None:
        print(f"  (rssi filter: min={a.rssi_min} max={a.rssi_max})")
    ok = all([
        check_seq_independence(frames),
        check_decorrelation(frames),
        check_constellation(frames),
    ])
    print("\nRESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
