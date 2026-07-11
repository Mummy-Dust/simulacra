#!/usr/bin/env python3
"""RPA-on-air presence check — attacker's-eye confirmation for the churn_adv raw-HCI fix.

Milestone A's headline decoy is the "privacy phone": a device advertising with a
Resolvable-Private-Address subtype (top-2-bits 01 / 0x40) that rotates. NimBLE's
`ble_gap_ext_adv_set_addr` REJECTS that subtype, so before the churn_adv raw-HCI fix
the decoy fleet emitted ZERO RPA-shaped addresses on air (a third of the roster was
silently dark). This script confirms, from a real BLE capture, that RPA-shaped
addresses are now actually transmitting.

Usage:
    python rpa_presence.py <capture.pcap> [min_rpa_advertisers]
    # default floor = 3 distinct RPA advertisers

It reuses capture_profile.py's parser (DLT256 + Nordic DLT157 aware) and counts
DISTINCT advertisers by address subtype. Exits non-zero if fewer than the floor of
RPA advertisers are present.

HONEST SCOPE: this counts ALL RPA advertisers in range — ambient real phones emit
RPA too, so a nonzero count is not by itself proof the DECOYS are emitting it. The
decisive signal is before-vs-after: with the fleet running, decoys were contributing
zero RPA before the fix; a healthy RPA count now (especially of short-lived, churning
advertisers co-located with the fleet) is the confirmation. For strict decoy
isolation, cross-reference strong-RSSI / co-located advertisers or decoy archetypes.
This script does NOT measure the physical layer (RSSI/AoA) — consistent with the
spec's honest ceiling, on-air RPA presence raises cost, it is not invisibility.
"""
import sys, os
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from capture_profile import parse_adverts   # noqa: E402


def main():
    if len(sys.argv) < 2:
        print("usage: python rpa_presence.py <capture.pcap> [min_rpa_advertisers]")
        sys.exit(2)
    path = sys.argv[1]
    floor = int(sys.argv[2]) if len(sys.argv) > 2 else 3

    adverts = parse_adverts(path)
    if not adverts:
        print(f"FAIL: no adverts parsed from {path}")
        sys.exit(1)

    # First-seen subtype per distinct advertiser address (capture_profile._atype:
    # 3->static, 1->rpa, 0->'public' == NRPA-or-public random, by address MSB).
    subtype = {}
    for a in adverts:
        subtype.setdefault(a["addr"], a["atype"])
    counts = Counter(subtype.values())
    total = len(subtype)

    print(f"adverts parsed:        {len(adverts)}")
    print(f"distinct advertisers:  {total}")
    for t in ("static", "rpa", "public"):
        n = counts.get(t, 0)
        label = "RPA (0x40)" if t == "rpa" else ("static (0xC0)" if t == "static" else "NRPA/public (0x00)")
        print(f"  {label:<20} {n:4d}  ({(100 * n // max(total, 1))}%)")

    rpa = counts.get("rpa", 0)
    print()
    if rpa >= floor:
        print(f"PASS: {rpa} distinct RPA-shaped advertisers on air (floor {floor}).")
        print("      Before the churn_adv fix, decoys emitted zero RPA (set_addr rejected 0x40);")
        print("      a healthy count with the fleet running confirms decoys now transmit RPA.")
        print("      NOTE: ambient phones also emit RPA - for strict decoy isolation cross-check")
        print("      strong-RSSI/co-located advertisers or decoy archetypes.")
        sys.exit(0)
    print(f"FAIL: only {rpa} RPA-shaped advertisers (< floor {floor}).")
    print("      If the fleet is running and co-located, the decoys may not be emitting RPA —")
    print("      check the serial for churn_adv 'rnd_addr rc' errors and confirm the fix is flashed.")
    sys.exit(1)


if __name__ == "__main__":
    main()
