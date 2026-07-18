#!/usr/bin/env python3
"""Behavioral probe audit scorecard: how separable are the Wi-Fi probe decoys from a real phone
crowd? Generator-first -- the decoy model comes from `probe_dump --agents` (the real probe_agents.c);
the real reference comes from a .kismet capture (analyzers/kismet_behavior.py). Headline = worst tell.

Approach A (single mixed capture): on a decoy-DOMINATED capture the blending axes UNDERSTATE (the
decoys are in the reference), so density_dominance is the meaningful number there. The tool always
prints the dominance ratio so a misleadingly-low blending score is self-evident."""
import argparse
import math
import os
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "analyzers"))
import kismet_behavior as kb   # noqa: E402

# multiplicity buckets: how many MACs share one probe fingerprint
MULT_BUCKETS = [1, 2, 3, 5, 9, 17, 33]   # lower edges; last is 33+


def mult_hist(counts):
    """Histogram a list of per-fingerprint MAC counts into MULT_BUCKETS -> list[len(MULT_BUCKETS)]."""
    h = [0] * len(MULT_BUCKETS)
    for c in counts:
        idx = 0
        for i, edge in enumerate(MULT_BUCKETS):
            if c >= edge:
                idx = i
        h[idx] += 1
    return h


def _norm(v):
    s = sum(v)
    return [x / s for x in v] if s > 0 else [0.0] * len(v)


def js_divergence(p, q):
    """Jensen-Shannon divergence of two histograms, in [0,1] (local copy; probe_audit stays self-contained)."""
    p = _norm(list(p)); q = _norm(list(q))
    m = [(pi + qi) / 2 for pi, qi in zip(p, q)]

    def kl(a, b):
        s = 0.0
        for ai, bi in zip(a, b):
            if ai > 0 and bi > 0:
                s += ai * math.log2(ai / bi)
        return s

    return max(0.0, min(1.0, 0.5 * kl(p, m) + 0.5 * kl(q, m)))


def decoy_profile_from_agents(rows):
    """rows = list of (arch, born_ms, wildcard, mac) from `probe_dump --agents` A-records."""
    per_arch = Counter(r[0] for r in rows)
    wc = sum(1 for r in rows if r[2])
    n = len(rows)
    return {"n_probing": n,
            "wildcard_fraction": (wc / n) if n else 0.0,
            "mult_counts": sorted(per_arch.values())}


def score(decoy, ref):
    dd = decoy["n_probing"] / ref["n_probing"] if ref["n_probing"] else 0.0
    dd = max(0.0, min(1.0, dd))
    wf = abs(decoy["wildcard_fraction"] - ref["wildcard_fraction"])
    fm = js_divergence(mult_hist(decoy["mult_counts"]), mult_hist(ref["mult_counts"]))
    axes = {"density_dominance": round(dd, 4),
            "wildcard_fraction": round(wf, 4),
            "fingerprint_multiplicity": round(fm, 4)}
    tell = max(axes, key=axes.get)
    return {**axes, "headline": axes[tell], "headline_tell": tell,
            "dominance_note": "mixed-reference upper bound"}


def run_decoy_model(exe, seed, n, ticks, tick_ms):
    out = subprocess.check_output([exe, "--agents", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 5 and p[0] == "A":
            rows.append((int(p[1]), int(p[2]), int(p[3]), p[4]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kismet", help="path to a .kismet capture (the real reference)")
    ap.add_argument("--agents", type=int, default=16, help="modeled concurrent probe agents (PROBE_PHONES)")
    ap.add_argument("--ticks", type=int, default=2220)
    ap.add_argument("--tick-ms", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--gate", type=float, default=1.1)
    a = ap.parse_args()

    tool = os.path.dirname(os.path.abspath(__file__))
    exe = os.path.join(tool, "probe_dump.exe" if os.name == "nt" else "probe_dump")
    if not os.path.exists(exe):
        sys.exit("probe_dump not built (run run.ps1 -Rebuild)")

    rows = run_decoy_model(exe, a.seed, a.agents, a.ticks, a.tick_ms)
    decoy = decoy_profile_from_agents(rows)
    ref = kb.reference_profile(a.kismet)
    card = score(decoy, ref)

    print("%-26s %12s" % ("BEHAVIORAL AXIS", "SEPARABILITY"))
    for k in ("density_dominance", "wildcard_fraction", "fingerprint_multiplicity"):
        print("%-26s %12.4f" % (k, card[k]))
    print("-" * 40)
    print("HEADLINE (max) %.4f  worst tell: %s" % (card["headline"], card["headline_tell"]))
    print("decoy-dominance: modeled %d of %d observed probing devices (%s) -- "
          "on a decoy-heavy capture the blending axes understate."
          % (decoy["n_probing"], ref["n_probing"], card["dominance_note"]))
    sys.exit(1 if card["headline"] > a.gate else 0)


if __name__ == "__main__":
    main()
