#!/usr/bin/env python3
"""Score the probe-behavioral-realism metrics from a Kismet pcapng (attacker's-eye view).

This is the external-observer complement to sniff_analyze.py: instead of our own C5 sniffer's
serial log, it reads what a passive surveillance tool would capture. Same three checks.

No scapy/tshark dependency -- hand-rolled pcapng walk: EPB blocks -> strip radiotap by its length
field -> keep probe requests (FC 0x40 0x00) -> SA = frame[10:16], seq = (u16le(frame[22:24])>>4).

  1. seq-independence  -- FAIL if any run of >=3 frames (radio-timestamp order) increments seq by
                          exactly 1 across changing MACs (one shared hardware counter).
  2. decorrelation     -- FAIL if any tight window shows a big simultaneous volley of distinct MACs.
  3. constellation     -- distinct MACs + first/last-third overlap; FAIL if the set never churns.

    python pcap_analyze.py <capture.pcapng>

Captures live only in the gitignored private/. NEVER commit one.
"""
import sys, struct
from collections import defaultdict


def blocks(d):
    o = 0
    while o + 12 <= len(d):
        t, l = struct.unpack_from("<II", d, o)
        if l < 12 or o + l > len(d):
            break
        yield t, d[o:o + l]
        o += l


def probes(path):
    d = open(path, "rb").read()
    out = []   # (ts, mac, seq)
    for t, blk in blocks(d):
        if t != 6:                              # Enhanced Packet Block
            continue
        ts = (struct.unpack_from("<I", blk, 8)[0] << 32) | struct.unpack_from("<I", blk, 12)[0]
        caplen = struct.unpack_from("<I", blk, 20)[0]
        pkt = blk[28:28 + caplen]
        if len(pkt) < 4:
            continue
        rtlen = struct.unpack_from("<H", pkt, 2)[0]     # radiotap length
        f = pkt[rtlen:]
        if len(f) < 24 or f[0] != 0x40 or f[1] != 0x00:
            continue
        mac = ":".join("%02x" % b for b in f[10:16])
        seq = (f[22] | (f[23] << 8)) >> 4
        out.append((ts, mac, seq))
    out.sort(key=lambda r: r[0])
    return out


def seq_independence(frames):
    run, worst, bad = 1, 0, []
    for (t0, m0, s0), (t1, m1, s1) in zip(frames, frames[1:]):
        if s1 == (s0 + 1) & 0x0FFF and m1 != m0:
            run += 1
            if run >= 3:
                bad.append((m0, s0, m1, s1))
        else:
            run = 1
        worst = max(worst, run)
    ok = not bad
    print(f"[1] seq-independence : {'PASS' if ok else 'FAIL'}  (longest cross-MAC +1 run={worst}; want <3)")
    for m0, s0, m1, s1 in bad[:4]:
        print(f"      shared-counter signature: {m0} seq={s0} -> {m1} seq={s1}")
    return ok


def decorrelation(frames, window_us=25000, volley=6):
    worst, worst_t, i = 0, 0, 0
    for j in range(len(frames)):
        t = frames[j][0]
        while frames[i][0] < t - window_us:
            i += 1
        n = len({frames[k][1] for k in range(i, j + 1)})
        if n > worst:
            worst, worst_t = n, t
    ok = worst < volley
    print(f"[2] decorrelation    : {'PASS' if ok else 'FAIL'}  "
          f"(max {worst} distinct MACs within {window_us//1000}ms; want <{volley})")
    return ok


def constellation(frames, overlap_max=0.6):
    macs = [f[1] for f in frames]
    n = len(frames)
    if n < 6:
        print(f"[3] constellation    : distinct MACs={len(set(macs))} (capture too short to judge churn)")
        return True
    a, b = set(macs[:n // 3]), set(macs[2 * n // 3:])
    jac = len(a & b) / len(a | b) if (a | b) else 1.0
    ok = jac <= overlap_max
    print(f"[3] constellation    : {'PASS' if ok else 'FAIL'}  "
          f"(distinct MACs={len(set(macs))}, first/last-third overlap={jac:.2f}; want <={overlap_max})")
    return ok


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    frames = probes(sys.argv[1])
    if not frames:
        print("no probe requests parsed")
        sys.exit(2)
    print(f"parsed {len(frames)} probe requests, {len({f[1] for f in frames})} distinct MACs")
    ok = all([seq_independence(frames), decorrelation(frames), constellation(frames)])
    print("\nRESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
