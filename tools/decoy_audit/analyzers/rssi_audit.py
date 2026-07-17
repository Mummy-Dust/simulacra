#!/usr/bin/env python3
"""Physical RSSI separability: how distinguishable is the decoys' RSSI *shape* from a real crowd's.
Placement-invariant -- each distribution is centered on its own median, so absolute level (where you
physically placed the boards) does not drive the score. Reuses the audit's Jensen-Shannon divergence.

    python rssi_audit.py --real real_profile.json --decoy-log observe_serial.log
    python rssi_audit.py --real real_profile.json --decoy-json decoy_profile.json
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from discriminators import rssi_separability   # noqa: E402  (shared comparator)
import capture_profile as cp                    # noqa: E402


def load_rssi(prof):
    """profile dict -> (bins, median, stdev, n) or None if it carries no RSSI."""
    if not prof or "rssi_bins" not in prof:
        return None
    return (prof["rssi_bins"], prof.get("rssi_median", 0.0),
            prof.get("rssi_stdev", 0.0), prof.get("n_rssi", 0))


def read_log_rssi(path):
    """OBSERVE/sniff serial log -> list of RSSI ints (parses 'rssi=<n>')."""
    rx = re.compile(r"rssi=(-?\d+)")
    out = []
    with open(path, encoding="utf-8", errors="ignore") as f:
        for ln in f:
            m = rx.search(ln)
            if m:
                out.append(int(m.group(1)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--real", required=True, help="real-crowd profile.json (with rssi_bins)")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--decoy-json", help="decoy profile.json (with rssi_bins)")
    g.add_argument("--decoy-log", help="OBSERVE serial log (parses rssi=<n>)")
    a = ap.parse_args()

    real = load_rssi(json.load(open(a.real)))
    if not real:
        print("real profile has no rssi_bins (re-run capture_profile on an RSSI-bearing pcap)")
        sys.exit(2)
    if a.decoy_json:
        decoy = load_rssi(json.load(open(a.decoy_json)))
    else:
        decoy = load_rssi(cp.rssi_hist(read_log_rssi(a.decoy_log)))
    if not decoy:
        print("decoy side has no usable RSSI (empty log / no rssi_bins)")
        sys.exit(2)

    sep = rssi_separability(decoy, real)
    ratio = decoy[2] / real[2] if real[2] else float("inf")
    read = "separable" if sep >= 0.5 else "marginal" if sep >= 0.2 else "indistinguishable"
    print(f"decoy : n={decoy[3]} median={decoy[1]:.0f} dBm stdev={decoy[2]:.1f} dB")
    print(f"real  : n={real[3]} median={real[1]:.0f} dBm stdev={real[2]:.1f} dB")
    print(f"spread ratio (decoy/real): {ratio:.2f}  (<<1 = decoys too tight)")
    print(f"rssi_separability (shape, placement-invariant): {sep:.3f}  -> {read}")
    sys.exit(0)


if __name__ == "__main__":
    main()
