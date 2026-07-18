#!/usr/bin/env python3
"""Parse a Kismet .kismet SQLite capture into a behavioral REFERENCE profile for the probe audit.

Emits ONLY counts, fractions, and fingerprint hashes -- never a real SSID, MAC, or coordinate.
The comparable population is randomized (locally-administered) MAC, probe-only (never-associated)
Wi-Fi clients -- i.e. the same kind of device the decoys emulate."""
import json
import sqlite3
from collections import Counter

_ZERO_BSSID = ("", "00:00:00:00:00:00")


def _is_la(mac):
    try:
        return bool(int(mac.split(":")[0], 16) & 0x02)
    except (ValueError, IndexError, AttributeError):
        return False


def _has_named_ssid(dot):
    for e in dot.get("dot11.device.probed_ssid_map", []) or []:
        if isinstance(e, dict) and e.get("dot11.probedssid.ssid", ""):
            return True
    return False


def reference_profile(kismet_path):
    """{'n_probing': int, 'wildcard_fraction': float, 'mult_counts': [int, ...]}."""
    db = sqlite3.connect(kismet_path)
    try:
        rows = db.execute("SELECT device FROM devices").fetchall()
    finally:
        db.close()

    n = 0
    wildcard = 0
    fp_macs = Counter()
    for (blob,) in rows:
        try:
            d = json.loads(blob)
        except (TypeError, json.JSONDecodeError):
            continue
        dot = d.get("dot11.device")
        if not dot:
            continue                                   # not a dot11 device
        mac = d.get("kismet.device.base.macaddr", "")
        if not _is_la(mac):
            continue                                   # only randomized MACs (decoy-comparable)
        bssid = dot.get("dot11.device.last_bssid", "") or ""
        if bssid not in _ZERO_BSSID:
            continue                                   # associated -> not a probe-only client
        n += 1
        if not _has_named_ssid(dot):
            wildcard += 1
        fp = dot.get("dot11.device.probe_fingerprint", 0)
        if fp:
            fp_macs[fp] += 1

    return {
        "n_probing": n,
        "wildcard_fraction": (wildcard / n) if n else 0.0,
        "mult_counts": sorted(fp_macs.values()),
    }
