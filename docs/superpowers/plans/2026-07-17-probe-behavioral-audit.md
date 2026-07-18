# Probe Behavioral Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A generator-first host tool that scores how separable the Wi-Fi probe decoys are from a real phone crowd on behavioral axes (density dominance, wildcard fraction, fingerprint multiplicity), from a `.kismet` capture.

**Architecture:** Extend `probe_dump --agents` to emit per-birth records from the real `probe_agents.c` (the labeled decoy model); a new `analyzers/kismet_behavior.py` parses a `.kismet` SQLite device table into a real reference profile; `probe_behavior_scorecard.py` scores the two against each other and prints a ranked scorecard, mirroring `tools/decoy_audit/scorecard.py`.

**Tech Stack:** C (MSVC `cl` for the host harness), Python 3.12 stdlib (`unittest`, `sqlite3`, `json`, `math`). No new dependencies.

## Global Constraints

- Everything lives under `tools/probe_audit/`. Python run via `"C:/Program Files/Python312/python.exe" -m pytest -q` from that dir.
- `cl` (MSVC) builds `probe_dump.exe`; run from a "Developer PowerShell for VS" (cl on PATH).
- **Approach A** — single mixed capture, conservative, *pluggable* reference. The tool prints its **decoy-dominance ratio alongside the scores** so a misleadingly-low blending score on a decoy-heavy capture is self-evident.
- **Privacy (hard):** the `.kismet` analyzer emits only counts, fractions, and fingerprint *hashes* — **never** a real SSID string, MAC, or coordinate. The capture stays in gitignored `private/`.
- **Never hardcode the OS username or forbidden committer name into any tracked file** (see `private/TOOLING-GOTCHAS.md`) — scan for absolute path prefixes only, refer to name-tokens indirectly.
- Archetypes: `ARCH_IPHONE=0, ARCH_GALAXY=1, ARCH_PIXEL=2, ARCH_ANDROID=3` (`PROBE_ARCH_COUNT=4`).
- Commit identity `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>`; every commit ends with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.
- Branch `feat/probe-behavioral-audit`; merge to `main` `--no-ff`, PII-scan the range, push at the end.

---

## File Structure

- **Modify** `tools/probe_audit/probe_dump.c` — extend `--agents` to also emit `A <arch> <born_ms> <wildcard> <mac>` birth records (non-breaking; existing `E` lines unchanged).
- **Modify** `tools/probe_audit/tests/test_probe_agents.py` — add a `Births` class for the new records.
- **Create** `tools/probe_audit/analyzers/kismet_behavior.py` — `.kismet` → reference behavioral profile.
- **Create** `tools/probe_audit/tests/test_kismet_behavior.py` — inline synthetic `.kismet` + skip-if-absent real check.
- **Create** `tools/probe_audit/probe_behavior_scorecard.py` — decoy model + reference + 3 axes + headline + `--gate`.
- **Create** `tools/probe_audit/tests/test_probe_behavior_scorecard.py` — per-axis on hand-built profiles.

**Profile shape (shared contract):** a dict `{"n_probing": int, "wildcard_fraction": float, "mult_counts": list[int]}` where `mult_counts` is the per-fingerprint MAC count list (unbucketed). Both the decoy model (from `A` records, grouped by archetype) and the reference (from `.kismet`, grouped by probe_fingerprint) produce this shape. The scorecard does the bucketing so it is defined once.

---

### Task 1: `probe_dump --agents` emits per-birth records

**Files:**
- Modify: `tools/probe_audit/probe_dump.c` (the `--agents` block, ~lines 56-77)
- Test: `tools/probe_audit/tests/test_probe_agents.py`

**Interfaces:**
- Produces: new stdout lines `A <arch:int> <born_ms:uint> <wildcard:int> <mac:12hex>`, one per agent birth (initial spawn + each reincarnation) over the simulated window. Existing `E <t> <mac> <seq>` lines are unchanged. Task 3 consumes the `A` lines.

- [ ] **Step 1: Write the failing test**

Add to `tools/probe_audit/tests/test_probe_agents.py` (new class + helper, before the `if __name__` guard):

```python
def births(seed, n=16, ticks=2000, tick_ms=1000):
    out = subprocess.check_output([EXE, "--agents", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    recs = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 5 and p[0] == "A":
            recs.append((int(p[1]), int(p[2]), int(p[3]), p[4]))   # arch, born_ms, wildcard, mac
    return recs


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Births(unittest.TestCase):
    def test_births_emitted_with_unique_macs(self):
        recs = births(2)
        self.assertGreater(len(recs), 16)                       # initial 16 + reincarnations
        macs = [r[3] for r in recs]
        self.assertEqual(len(macs), len(set(macs)), "a birth reused a MAC (uniqueness broken)")

    def test_wildcard_flag_is_one_today(self):
        self.assertTrue(all(r[2] == 1 for r in births(3)))       # all probes wildcard today

    def test_archetypes_in_range(self):
        self.assertTrue(all(0 <= r[0] < 4 for r in births(4)))   # PROBE_ARCH_COUNT == 4

    def test_multiplicity_per_archetype(self):
        from collections import Counter
        per_arch = Counter(r[0] for r in births(5, ticks=2000, tick_ms=2000))
        self.assertTrue(any(v >= 3 for v in per_arch.values()),
                        "no archetype churned multiple MACs (multiplicity axis would be empty)")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_probe_agents.py -q -k Births`
Expected: FAIL — no `A` lines yet, so `births()` returns `[]` and `assertGreater(0, 16)` fails.

- [ ] **Step 3: Extend the `--agents` block in `tools/probe_audit/probe_dump.c`**

Replace the current `--agents` block (from `if (argc > 1 && strcmp(argv[1], "--agents") == 0) {` through its `return 0;`) with:

```c
    if (argc > 1 && strcmp(argv[1], "--agents") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      nag    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 8;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 2000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 2000;
        srand(seed);
        uint32_t t = 0;
        probe_agents_init(nag, t);
        uint32_t last_born[PROBE_AGENTS_MAX];
        for (int i = 0; i < PROBE_AGENTS_MAX; i++) last_born[i] = 0u;
        // A record: one per (re)born agent identity -> arch, born_ms, wildcard(=1 today), mac
        for (int i = 0; i < probe_agents_count(); i++) {
            const probe_agent_t *a = probe_agents_at(i);
            printf("A %d %u 1 ", (int)a->arch, (unsigned)a->born_ms);
            for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
            printf("\n");
            last_born[i] = a->born_ms;
        }
        for (int s = 0; s < ticks; s++) {
            t += tickms;
            probe_agents_lifecycle(t);
            for (int i = 0; i < probe_agents_count(); i++) {
                const probe_agent_t *a = probe_agents_at(i);
                if (a->born_ms != last_born[i]) {               // reincarnated this tick
                    printf("A %d %u 1 ", (int)a->arch, (unsigned)a->born_ms);
                    for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
                    printf("\n");
                    last_born[i] = a->born_ms;
                }
            }
            probe_agent_t *due[PROBE_AGENTS_MAX];
            int nd = probe_agents_due(t, due, PROBE_AGENTS_MAX);
            for (int i = 0; i < nd; i++) {
                uint16_t sq = probe_agent_next_seq(due[i]);
                printf("E %u ", (unsigned)t);
                for (int b = 0; b < 6; b++) printf("%02x", due[i]->mac[b]);
                printf(" %u\n", (unsigned)sq);
            }
        }
        return 0;
    }
```

- [ ] **Step 4: Rebuild `probe_dump.exe`**

From a Developer PowerShell for VS, in `tools/probe_audit`:

```powershell
.\run.ps1 -Rebuild
```

(If `run.ps1` has no `-Rebuild`, build directly with the `cl` line from `tools/probe_audit/README.md`.) Expected: compiles clean, `probe_dump.exe` regenerated.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_probe_agents.py -q`
Expected: PASS — the new `Births` class plus all existing `Agents` tests (the `E`-line tests are unaffected).

- [ ] **Step 6: Commit**

```bash
git add tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_probe_agents.py
git commit -F - <<'EOF'
feat(probe_audit): --agents emits per-birth records (arch, born, wildcard, mac)

Adds A-records (one per agent (re)birth) alongside the existing E emission
lines, so the behavioral audit can model decoy device count, per-archetype
MAC multiplicity, and wildcard fraction. Non-breaking: E lines unchanged.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 2: `.kismet` reference profile parser

**Files:**
- Create: `tools/probe_audit/analyzers/kismet_behavior.py`
- Test: `tools/probe_audit/tests/test_kismet_behavior.py`

**Interfaces:**
- Produces: `reference_profile(kismet_path) -> dict` = `{"n_probing": int, "wildcard_fraction": float, "mult_counts": list[int]}`, computed over **randomized (locally-administered) MAC, probe-only (never-associated)** Wi-Fi clients. `mult_counts` = per-`probe_fingerprint` MAC counts (fingerprint 0 excluded). Task 3 consumes this.

- [ ] **Step 1: Write the failing test**

Create `tools/probe_audit/tests/test_kismet_behavior.py`:

```python
import json, os, sqlite3, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TOOL, "analyzers"))
import kismet_behavior as kb   # noqa: E402


def _dev(mac, fp=0, named=None, assoc=False):
    """Build a minimal Kismet device JSON blob. `named`=a list of probed SSID strings (None=wildcard)."""
    pm = []
    if named is not None:
        pm = [{"dot11.probedssid.ssid": s} for s in named]
    else:
        pm = [{"dot11.probedssid.ssid": ""}]   # wildcard entry
    return json.dumps({
        "kismet.device.base.macaddr": mac,
        "dot11.device": {
            "dot11.device.probe_fingerprint": fp,
            "dot11.device.probed_ssid_map": pm,
            "dot11.device.last_bssid": "11:22:33:44:55:66" if assoc else "00:00:00:00:00:00",
        },
    })


def _write_kismet(path, blobs):
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE devices (device TEXT)")
    db.executemany("INSERT INTO devices (device) VALUES (?)", [(b,) for b in blobs])
    db.commit(); db.close()


class KismetBehavior(unittest.TestCase):
    def setUp(self):
        self.path = tempfile.NamedTemporaryFile(suffix=".kismet", delete=False).name
        blobs = [
            _dev("02:00:00:00:00:01", fp=111),                        # LA wildcard, fp 111
            _dev("02:00:00:00:00:02", fp=111),                        # LA wildcard, fp 111 (mult=2)
            _dev("02:00:00:00:00:03", fp=222, named=["homenet"]),      # LA named -> not wildcard
            _dev("02:00:00:00:00:04", fp=333, assoc=True),            # LA but associated -> excluded
            _dev("aa:bb:cc:00:00:05", fp=444),                        # global MAC -> excluded
        ]
        _write_kismet(self.path, blobs)

    def tearDown(self):
        os.remove(self.path)

    def test_counts_only_randomized_probe_only(self):
        prof = kb.reference_profile(self.path)
        self.assertEqual(prof["n_probing"], 3)          # macs 01,02,03 (04 assoc, 05 global excluded)

    def test_wildcard_fraction(self):
        prof = kb.reference_profile(self.path)
        self.assertAlmostEqual(prof["wildcard_fraction"], 2/3, places=6)   # 01,02 wildcard; 03 named

    def test_multiplicity_counts(self):
        prof = kb.reference_profile(self.path)
        self.assertEqual(sorted(prof["mult_counts"]), [1, 2])   # fp111 has 2 macs, fp222 has 1

    def test_emits_no_pii(self):
        # the profile must not carry SSID strings or MACs
        prof = kb.reference_profile(self.path)
        blob = json.dumps(prof)
        self.assertNotIn("homenet", blob)
        self.assertNotIn("02:00:00", blob)


_REAL = os.path.join(TOOL, "..", "..", "private", "Kismetscannew.kismet")


@unittest.skipUnless(os.path.exists(_REAL), "private/Kismetscannew.kismet not present")
class RealCapture(unittest.TestCase):
    def test_sane_ranges(self):
        prof = kb.reference_profile(_REAL)
        self.assertGreater(prof["n_probing"], 0)
        self.assertTrue(0.0 <= prof["wildcard_fraction"] <= 1.0)
        self.assertTrue(all(c >= 1 for c in prof["mult_counts"]))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_kismet_behavior.py -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'kismet_behavior'`.

- [ ] **Step 3: Implement `tools/probe_audit/analyzers/kismet_behavior.py`**

```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_kismet_behavior.py -q`
Expected: PASS (4 synthetic tests; the `RealCapture` class runs if the capture is present, else skips).

- [ ] **Step 5: Commit**

```bash
git add tools/probe_audit/analyzers/kismet_behavior.py tools/probe_audit/tests/test_kismet_behavior.py
git commit -F - <<'EOF'
feat(probe_audit): .kismet -> behavioral reference profile (PII-safe)

reference_profile() extracts, over randomized probe-only clients, the count,
wildcard fraction, and per-fingerprint MAC multiplicity from a Kismet capture.
Emits only counts/fractions/fingerprint-hashes -- never SSID/MAC/coordinates.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 3: behavioral scorecard

**Files:**
- Create: `tools/probe_audit/probe_behavior_scorecard.py`
- Test: `tools/probe_audit/tests/test_probe_behavior_scorecard.py`

**Interfaces:**
- Consumes: `probe_dump --agents` `A` lines (Task 1); `kismet_behavior.reference_profile` (Task 2).
- Produces: `MULT_BUCKETS`, `mult_hist(counts) -> list[int]`, `js_divergence(p, q) -> float`, `decoy_profile_from_agents(rows) -> profile`, `score(decoy, ref) -> dict` (keys `density_dominance`, `wildcard_fraction`, `fingerprint_multiplicity`, `headline`, `headline_tell`, `dominance_note`), and a CLI.

- [ ] **Step 1: Write the failing test**

Create `tools/probe_audit/tests/test_probe_behavior_scorecard.py`:

```python
import os, sys, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)
import probe_behavior_scorecard as S   # noqa: E402


class Axes(unittest.TestCase):
    def test_density_dominance_is_count_ratio(self):
        decoy = {"n_probing": 30, "wildcard_fraction": 1.0, "mult_counts": [10, 10, 10]}
        ref   = {"n_probing": 40, "wildcard_fraction": 0.8, "mult_counts": [1, 1, 2]}
        card = S.score(decoy, ref)
        self.assertAlmostEqual(card["density_dominance"], 30/40, places=6)

    def test_wildcard_axis_is_abs_diff(self):
        decoy = {"n_probing": 10, "wildcard_fraction": 1.0, "mult_counts": [5]}
        ref   = {"n_probing": 10, "wildcard_fraction": 0.6, "mult_counts": [5]}
        self.assertAlmostEqual(S.score(decoy, ref)["wildcard_fraction"], 0.4, places=6)

    def test_multiplicity_zero_when_shapes_match(self):
        prof = {"n_probing": 8, "wildcard_fraction": 1.0, "mult_counts": [9, 9]}
        self.assertLess(S.score(prof, dict(prof))["fingerprint_multiplicity"], 1e-9)

    def test_multiplicity_positive_when_shapes_differ(self):
        decoy = {"n_probing": 20, "wildcard_fraction": 1.0, "mult_counts": [20]}        # one huge cluster
        ref   = {"n_probing": 20, "wildcard_fraction": 1.0, "mult_counts": [1]*20}       # 20 singletons
        self.assertGreater(S.score(decoy, ref)["fingerprint_multiplicity"], 0.3)

    def test_headline_is_max(self):
        decoy = {"n_probing": 30, "wildcard_fraction": 1.0, "mult_counts": [20]}
        ref   = {"n_probing": 40, "wildcard_fraction": 0.5, "mult_counts": [1]*20}
        card = S.score(decoy, ref)
        self.assertEqual(card["headline"], max(card["density_dominance"],
                                               card["wildcard_fraction"],
                                               card["fingerprint_multiplicity"]))
        self.assertIn(card["headline_tell"],
                      ("density_dominance", "wildcard_fraction", "fingerprint_multiplicity"))

    def test_decoy_profile_from_agents(self):
        # A-records: (arch, born, wildcard, mac); 3 of arch 0, 1 of arch 1
        rows = [(0, 100, 1, "a"), (0, 200, 1, "b"), (0, 300, 1, "c"), (1, 100, 1, "d")]
        prof = S.decoy_profile_from_agents(rows)
        self.assertEqual(prof["n_probing"], 4)
        self.assertEqual(prof["wildcard_fraction"], 1.0)
        self.assertEqual(sorted(prof["mult_counts"]), [1, 3])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_probe_behavior_scorecard.py -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'probe_behavior_scorecard'`.

- [ ] **Step 3: Implement `tools/probe_audit/probe_behavior_scorecard.py`**

```python
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
            "dominance_note": "mixed-reference upper bound" }


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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_probe_behavior_scorecard.py -q`
Expected: PASS (6 tests). Then the whole suite: `"C:/Program Files/Python312/python.exe" -m pytest -q` → all pass (was 20; now ~30).

- [ ] **Step 5: Run it on the real capture (record the numbers)**

```powershell
"C:/Program Files/Python312/python.exe" probe_behavior_scorecard.py ..\..\private\Kismetscannew.kismet
```

Expected: prints the three axes + headline + the decoy-dominance line. Note the numbers — density_dominance should be high (decoy-heavy home), the blending axes lower (understated, as designed). These are the baseline the Wi-Fi population-match fix will later move.

- [ ] **Step 6: Commit**

```bash
git add tools/probe_audit/probe_behavior_scorecard.py tools/probe_audit/tests/test_probe_behavior_scorecard.py
git commit -F - <<'EOF'
feat(probe_audit): behavioral scorecard (density / wildcard / multiplicity)

Scores decoy-vs-real Wi-Fi probe separability: density_dominance (modeled decoy
count / observed probing devices), wildcard_fraction, fingerprint_multiplicity
(JS over MACs-per-fingerprint). Headline = worst tell; prints the decoy-
dominance ratio so a low blending score on a decoy-heavy capture is self-evident.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

## Finishing

After all tasks pass and the full `tools/probe_audit` suite is green, use **superpowers:finishing-a-development-branch**:
- Verify tests, merge `feat/probe-behavioral-audit` to `main` `--no-ff`.
- PII-scan the merged range (`C:\Users`, `/Users/`, `/home/`, the OS username, real MACs) before pushing.
- Push `main`.
- Update `private/PROJECT-MAP.md` §11 and `private/KISMET-VALIDATION.md` with the tool + measured baseline; note in memory that the probe behavioral audit exists.

## Self-Review

**Spec coverage:** (1) decoy model via extended `--agents` → Task 1. (2) `.kismet` reference parser, PII-safe → Task 2. (3) scorecard with density_dominance / wildcard_fraction / fingerprint_multiplicity, headline, `--gate`, dominance note → Task 3. Approach A + honest caveat surfaced in output → Task 3 `main()` dominance line. Local `js_divergence` (no cross-tool import) → Task 3. All spec sections map to a task.

**Placeholder scan:** no TBD/TODO; every code step is complete. Task 3 Step 5's "note the numbers" is a measurement action, not a code placeholder.

**Type consistency:** the profile dict `{"n_probing", "wildcard_fraction", "mult_counts"}` is identical across Tasks 2 and 3; `reference_profile`, `decoy_profile_from_agents`, `mult_hist`, `js_divergence`, `score` names match between their definitions (Task 2/3) and the tests. `A`-record field order `(arch, born_ms, wildcard, mac)` is consistent between Task 1's emitter, Task 1's `births()` parser, and Task 3's `run_decoy_model`/`decoy_profile_from_agents`.
