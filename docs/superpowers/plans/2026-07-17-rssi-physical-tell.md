# RSSI/TX Physical-Tell Audit Slice — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the decoy detectability audit's physical (RSSI) tell a first-class, repeatable scorecard discriminator by fixing DLT256 capture parsing and adding a modeled, single-node decoy-RSSI slice scored against a real crowd.

**Architecture:** Fix `capture_profile.py` so RSSI-bearing DLT256 captures parse (search past the PHDR's reference Access Address). Model decoy RSSI host-side as per-identity `tx_power` dither + seeded Gaussian jitter (single-node worst case — no spatial spread). Reuse the existing placement-invariant `rssi_separability()` (moved into `discriminators.py` for sharing). Wire the tell into `scorecard.py`'s `build_scorecard` (like `presence_duration`) so it is headline-eligible and labeled `visibility=modeled`.

**Tech Stack:** Python 3.12 (`C:/Program Files/Python312/python.exe`), pytest/unittest, MSVC `cl` for the C `synth_dump` (unchanged here). All host-side; no firmware changes.

## Global Constraints

- Public repo `github.com/Mummy-Dust/simulacra`. Commit identity **must** be `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>` (the repo's local git config already is this — plain `git commit` is correct).
- **Never commit** the OS username, absolute `C:\Users\<name>` paths, real MACs, real SSIDs, or anything under `private/` (it is gitignored; captures live there).
- Every commit ends with these two trailers, verbatim:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`
- Tests run with `"C:/Program Files/Python312/python.exe" -m pytest` from `tools/decoy_audit/`.
- The `.pcap` test fixtures are gitignored — build any new capture bytes inline in the test (temp file), do not add a committed binary fixture.
- Merge to `main` locally with `git merge --no-ff`; **no push**.

## Deviations from the spec (intentional, both simplifications)

1. Spec said "add `d_rssi` to `DISCRIMINATORS`." Instead, wire it in `scorecard.py::build_scorecard` (the same place `presence_duration` is added), because `score_all` hardcodes `visibility="logic"` and `test_discriminators.py::test_all_scores_in_unit_range` asserts every `score_all` row is `"logic"`. This keeps that invariant and lets the RSSI row carry `visibility="modeled"`.
2. Spec listed a `run.ps1` "rssi_physical line." No `run.ps1` code change is needed: it already prints every row `build_scorecard` returns, so the row appears automatically. Task 4 only *verifies* this.

## File Structure

- `tools/decoy_audit/capture_profile.py` — DLT256 AA-offset fix (Task 1).
- `tools/decoy_audit/discriminators.py` — home of the shared `rssi_separability`/`_median_bin` (Task 2) and the new modeled-RSSI logic `modeled_decoy_rssi_hist` + `d_rssi` (Task 3). Gains `import capture_profile as cp`.
- `tools/decoy_audit/analyzers/rssi_audit.py` — imports the moved comparator instead of defining it (Task 2). No behavior change.
- `tools/decoy_audit/scorecard.py` — append the `rssi_physical` row in `build_scorecard` (Task 4).
- `tools/decoy_audit/tests/test_capture_profile_dlt256.py` — new, DLT256 fix tests (Task 1).
- `tools/decoy_audit/tests/test_discriminators_rssi.py` — new, modeled-RSSI unit tests (Task 3).
- `tools/decoy_audit/tests/test_scorecard.py` — extend with RSSI-row tests (Task 4).
- `tools/decoy_audit/README.md` — document the discriminator + modeled caveat (Task 5).

---

### Task 1: Fix DLT256 capture parsing

**Files:**
- Modify: `tools/decoy_audit/capture_profile.py` (function `parse_adverts`, ~line 81)
- Create: `tools/decoy_audit/tests/test_capture_profile_dlt256.py`

**Interfaces:**
- Consumes: `capture_profile.parse_adverts(path) -> list[dict]`, `capture_profile.build_profile(adverts) -> dict` (existing).
- Produces: no new public symbols; `parse_adverts` now correctly parses `linktype == 256` records whose PHDR carries a reference Access Address at offset 4.

**Background:** In DLT256 (LE LL *with PHDR*) each record is `[10-byte PHDR][packet AA (4)][PDU]`. The PHDR contains a *reference* copy of the advertising Access Address `d6 be 89 8e` at offset 4, so `data.find(AA)` stops at offset 4 instead of the real packet AA at offset 10; the PDU is then read 6 bytes early and dropped. The existing gitignored `sample_dlt256.pcap` fixture has an **all-zero** PHDR (no reference AA), so it accidentally parses and never exposed this. The fix: for `linktype == 256`, start the search past the PHDR reference AA.

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_capture_profile_dlt256.py`:

```python
import os, struct, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)
import capture_profile as cp  # noqa: E402

AA = bytes.fromhex("d6be898e")


def _dlt256_realistic_record(adva6, ad, sig_dbm=-55):
    """A DLT256 (LE LL w/ PHDR) record shaped like a real capture:
    PHDR = RF-chan, signal-power(signed), noise, AA-offenses, REFERENCE-AA(4), flags(2);
    then the real packet AA(4) and the ADV_NONCONN_IND PDU. The reference AA at offset 4
    is what makes a naive find(AA) misfire."""
    phdr = bytearray(10)
    phdr[0] = 0x25                       # RF channel
    phdr[1] = sig_dbm & 0xFF             # signal power (signed) -> RSSI
    phdr[2] = 0x80                       # noise -128
    phdr[3] = 0x00                       # AA offenses
    phdr[4:8] = AA                       # REFERENCE Access Address (the trap)
    phdr[8:10] = b"\x13\x0c"             # flags
    pdu = bytes([0x02, 6 + len(ad)]) + adva6 + ad   # ADV_NONCONN_IND
    return bytes(phdr) + AA + pdu


def _write_pcap(path, records, linktype):
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, linktype))
        for r in records:
            f.write(struct.pack("<IIII", 0, 0, len(r), len(r)))
            f.write(r)


class DLT256(unittest.TestCase):
    def setUp(self):
        self.path = tempfile.NamedTemporaryFile(suffix=".pcap", delete=False).name
        advas = [(bytes([1, 2, 3, 4, 5, 0xC0]), 0x0075),
                 (bytes([9, 9, 9, 9, 9, 0xD0]), 0x0087),
                 (bytes([1, 2, 3, 4, 5, 0x40]), 0x004C)]   # C0/D0 static, 40 RPA
        mfg = lambda c: bytes([0x02, 0x01, 0x06, 0x04, 0xFF, c & 0xFF, c >> 8, 0x11])
        _write_pcap(self.path, [_dlt256_realistic_record(a, mfg(c)) for a, c in advas], 256)

    def tearDown(self):
        os.remove(self.path)

    def test_realistic_dlt256_parses_past_reference_aa(self):
        adverts = cp.parse_adverts(self.path)
        self.assertEqual(len(adverts), 3, "reference AA in PHDR must not swallow the record")

    def test_realistic_dlt256_populates_rssi(self):
        prof = cp.build_profile(cp.parse_adverts(self.path))
        self.assertIn("rssi_bins", prof)
        self.assertGreater(prof["n_rssi"], 0)
        self.assertTrue(-110 <= prof["rssi_median"] <= -20)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tests/test_capture_profile_dlt256.py -v`
Expected: FAIL — `test_realistic_dlt256_parses_past_reference_aa` gets `0 != 3` (the reference AA at offset 4 makes every record misparse and drop).

- [ ] **Step 3: Apply the fix**

In `tools/decoy_audit/capture_profile.py`, inside `parse_adverts`, change the AA search (currently `off=data.find(AA)`):

```python
            # DLT256 (LE LL w/ PHDR) carries a *reference* copy of the advertising AA at record
            # offset 4; the real packet AA is at offset 10. Search past the PHDR so we lock onto
            # the packet AA, not the reference. DLT157 (Nordic) has a single AA -> search from 0.
            off = data.find(AA, 8) if linktype == 256 else data.find(AA)
            if off<0 or off+6>len(data): continue
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tests/test_capture_profile_dlt256.py -v`
Expected: PASS (both tests).

- [ ] **Step 5: Add the real-capture integration test (skips if absent)**

Append this class to `tools/decoy_audit/tests/test_capture_profile_dlt256.py`:

```python
_NEWLONG = os.path.join(TOOL, "..", "..", "private", "newlong.pcap")


@unittest.skipUnless(os.path.exists(_NEWLONG), "private/newlong.pcap not present")
class NewlongRealCapture(unittest.TestCase):
    def test_parses_with_rssi(self):
        prof = cp.build_profile(cp.parse_adverts(_NEWLONG))
        self.assertGreater(prof["n_adverts"], 0)
        self.assertGreater(prof["n_rssi"], 0)
        self.assertTrue(5.0 <= prof["rssi_stdev"] <= 20.0,
                        f"real-crowd RSSI stdev out of sane band: {prof['rssi_stdev']}")
```

- [ ] **Step 6: Run the full capture-profile test set**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tests/test_capture_profile_dlt256.py tests/test_capture_profile.py tests/test_capture_profile_rssi.py -v`
Expected: PASS. Note `test_capture_profile_rssi.py::test_parse_dlt256_sample_omits_rssi` still passes — the zero-PHDR fixture's AA is at offset 10, and `find(AA, 8)` still lands there.

- [ ] **Step 7: Commit**

```bash
git add tools/decoy_audit/capture_profile.py tools/decoy_audit/tests/test_capture_profile_dlt256.py
git commit -m "$(cat <<'EOF'
fix(decoy_audit): parse DLT256 captures past the PHDR reference AA

LE LL w/ PHDR records carry a reference copy of the advertising Access Address
at offset 4; find(AA) locked onto it instead of the real packet AA at offset 10,
so every PDU was read 6 bytes early and dropped (0 adverts). Search from offset 8
for linktype 256 so RSSI-bearing captures (e.g. the DLT256 bench capture) parse.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 2: Share `rssi_separability` in `discriminators.py` (DRY)

**Files:**
- Modify: `tools/decoy_audit/discriminators.py` (add import + moved functions)
- Modify: `tools/decoy_audit/analyzers/rssi_audit.py` (import instead of define)
- Test: existing `tools/decoy_audit/tests/test_rssi_audit.py` must still pass; add one assertion.

**Interfaces:**
- Produces: `discriminators._median_bin(median) -> int`, `discriminators.rssi_separability(decoy, real) -> float` where `decoy`/`real` are indexable with `[0]=bins (list[float])`, `[1]=median (float)`. Both reuse `discriminators.js_divergence` and `capture_profile.RSSI_LO/RSSI_W/RSSI_NBINS`.
- Consumes: `capture_profile` constants (via new `import capture_profile as cp`). No import cycle: `capture_profile` imports only stdlib.

- [ ] **Step 1: Write the failing test**

Append to `tools/decoy_audit/tests/test_rssi_audit.py`:

```python
def test_separability_shared_from_discriminators():
    # rssi_audit must reuse discriminators.rssi_separability, not its own copy.
    import discriminators as Dmod
    assert ra.rssi_separability is Dmod.rssi_separability
```

- [ ] **Step 2: Run it to verify it fails**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tests/test_rssi_audit.py::test_separability_shared_from_discriminators -v`
Expected: FAIL (`AttributeError: module 'discriminators' has no attribute 'rssi_separability'`).

- [ ] **Step 3: Move the functions into `discriminators.py`**

In `tools/decoy_audit/discriminators.py`, add the import near the top (after `from collections import Counter`):

```python
import capture_profile as cp
```

Then add these two functions (place them just above `def synth_distributions`):

```python
def _median_bin(median):
    idx = int((median - cp.RSSI_LO) // cp.RSSI_W)
    return max(0, min(cp.RSSI_NBINS - 1, idx))

def rssi_separability(decoy, real):
    """JS-divergence of two median-centered RSSI shapes, in [0,1]. Placement-invariant:
    each distribution is aligned on its own median, so absolute level does not drive the score."""
    db, dm = decoy[0], decoy[1]
    rb, rm = real[0], real[1]
    dc, rc = _median_bin(dm), _median_bin(rm)
    dd = {i - dc: w for i, w in enumerate(db)}
    rr = {i - rc: w for i, w in enumerate(rb)}
    keys = sorted(set(dd) | set(rr))
    return js_divergence([dd.get(k, 0.0) for k in keys], [rr.get(k, 0.0) for k in keys])
```

- [ ] **Step 4: Point `rssi_audit.py` at the shared copy**

In `tools/decoy_audit/analyzers/rssi_audit.py`, change the existing import line (currently `from discriminators import js_divergence   # noqa: E402`) to import the shared comparator instead:

```python
from discriminators import rssi_separability   # noqa: E402  (shared comparator)
```

Leave the existing `import capture_profile as cp` line as-is (still used by `main`). Then delete the now-duplicate `_median_bin(median)` and `rssi_separability(decoy, real)` function definitions from `rssi_audit.py` (they now live in `discriminators.py`). Leave `load_rssi`, `read_log_rssi`, and `main` untouched.

- [ ] **Step 5: Run the RSSI-audit + discriminator tests**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tests/test_rssi_audit.py tests/test_discriminators.py -v`
Expected: PASS (all — including the pre-existing `test_narrow_vs_broad_is_separable`, `test_placement_invariant`, and the new `test_separability_shared_from_discriminators`).

- [ ] **Step 6: Commit**

```bash
git add tools/decoy_audit/discriminators.py tools/decoy_audit/analyzers/rssi_audit.py tools/decoy_audit/tests/test_rssi_audit.py
git commit -m "$(cat <<'EOF'
refactor(decoy_audit): share rssi_separability from discriminators (DRY)

Move the placement-invariant rssi_separability/_median_bin comparator out of
analyzers/rssi_audit.py into discriminators.py so the scorecard can reuse it for
a modeled-RSSI tell. rssi_audit imports it; no behavior change.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 3: Model decoy RSSI + `d_rssi` discriminator

**Files:**
- Modify: `tools/decoy_audit/discriminators.py` (constants + `modeled_decoy_rssi_hist` + `d_rssi`)
- Create: `tools/decoy_audit/tests/test_discriminators_rssi.py`

**Interfaces:**
- Consumes: `discriminators.rssi_separability` (Task 2), `capture_profile.rssi_hist(values) -> dict|None` (existing; keys `rssi_bins`, `rssi_median`, `rssi_stdev`, `n_rssi`).
- Produces:
  - `discriminators.RSSI_SIGMA_DB = 4.0`, `discriminators.RSSI_MODEL_SEED = 1337`, `discriminators.RSSI_MODEL_BASE = -60`.
  - `discriminators.modeled_decoy_rssi_hist(synth, seed=RSSI_MODEL_SEED, sigma=RSSI_SIGMA_DB, base=RSSI_MODEL_BASE) -> dict|None` — models single-node decoy RSSI as `base + tx_i + N(0, sigma)` per row that has a numeric `tx`; returns a `rssi_hist`-shaped dict, or `None` if no row carries `tx`.
  - `discriminators.d_rssi(synth, profile) -> float` — `rssi_separability(modeled, real)` when `profile["rssi_bins"]` exists and the model is non-empty, else `0.0`.

**Rationale for the numbers:** the real generator dithers tx over `{-12,-9,-6,-3,0,+3}` dBm (stdev ≈ 5.1 dB). Adding fixed-position multipath jitter σ = 4 dB gives a modeled single-node spread √(5.1²+4²) ≈ 6.5 dB — below the measured over-air 10.1 dB (which also had board-spread + ambient bleed), so σ is anchored, not inflated. The always-on jitter keeps the histogram non-degenerate even if tx were constant, which is what makes this a valid regression gate (drop the dither → spread stays ~σ, still far below a real ~12 dB crowd → tell stays high).

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_discriminators_rssi.py`:

```python
import os, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)
import discriminators as D  # noqa: E402
import capture_profile as cp  # noqa: E402

DITHER = [-12, -9, -6, -3, 0, 3]                       # the real generator's tx set


def _synth(tx_values):
    return [{"atype": "static", "itvl_ms": 150, "company": 0, "tx": t} for t in tx_values]


class ModeledRssi(unittest.TestCase):
    def test_hist_stdev_matches_dither_plus_jitter(self):
        # uniform over the real dither set (stdev ~5.1) + sigma=4 jitter -> ~6.5 dB, deterministic.
        synth = _synth(DITHER * 1000)
        h = D.modeled_decoy_rssi_hist(synth)
        self.assertIsNotNone(h)
        self.assertTrue(5.6 <= h["rssi_stdev"] <= 7.4, f"modeled stdev {h['rssi_stdev']}")

    def test_deterministic(self):
        synth = _synth(DITHER * 50)
        self.assertEqual(D.modeled_decoy_rssi_hist(synth), D.modeled_decoy_rssi_hist(synth))

    def test_no_tx_rows_returns_none(self):
        self.assertIsNone(D.modeled_decoy_rssi_hist([{"atype": "static", "itvl_ms": 150}]))

    def test_d_rssi_zero_when_profile_has_no_rssi(self):
        self.assertEqual(D.d_rssi(_synth(DITHER * 10), {"atype": {}}), 0.0)

    def test_d_rssi_separates_tight_decoys_from_broad_real(self):
        # decoys tight (dither+jitter ~6.5 dB) vs a broad real crowd (~17 dB) -> a real tell.
        broad = cp.rssi_hist(list(range(-90, -40)))     # ~50 dB uniform, stdev ~14.5
        profile = {"rssi_bins": broad["rssi_bins"], "rssi_median": broad["rssi_median"]}
        sep = D.d_rssi(_synth(DITHER * 200), profile)
        self.assertTrue(0.0 < sep <= 1.0, f"expected a real in-range tell, got {sep}")
        self.assertGreater(sep, 0.15)

    def test_d_rssi_bounded_with_constant_tx(self):
        # even with no dither (constant tx), jitter keeps it finite & bounded (regression-gate safe).
        broad = cp.rssi_hist(list(range(-90, -40)))
        profile = {"rssi_bins": broad["rssi_bins"], "rssi_median": broad["rssi_median"]}
        sep = D.d_rssi(_synth([0] * 500), profile)
        self.assertTrue(0.0 <= sep <= 1.0)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tests/test_discriminators_rssi.py -v`
Expected: FAIL (`AttributeError: module 'discriminators' has no attribute 'modeled_decoy_rssi_hist'`).

- [ ] **Step 3: Implement the model**

In `tools/decoy_audit/discriminators.py`, add `import random` at the top (next to `import math`). Then add, just below the `rssi_separability` function from Task 2:

```python
# --- modeled physical (RSSI) tell -------------------------------------------------------------
# Single-node worst case: all decoys emit from one antenna, so their RSSI spread comes only from
# the generator's per-identity tx-power dither plus fixed-position multipath jitter -- no spatial
# spread. sigma is anchored to the real over-air capture (modeled ~6.5 dB stdev < measured 10.1 dB).
RSSI_SIGMA_DB  = 4.0     # fixed-position multipath jitter (dB), the one physical unknown
RSSI_MODEL_SEED = 1337   # deterministic jitter so the tell is reproducible across runs
RSSI_MODEL_BASE = -60    # co-location reference level; irrelevant to the score (median-centered)

def modeled_decoy_rssi_hist(synth, seed=RSSI_MODEL_SEED, sigma=RSSI_SIGMA_DB, base=RSSI_MODEL_BASE):
    """Model single-node decoy RSSI from each identity's tx dither + N(0,sigma) jitter.
    Returns a capture_profile.rssi_hist-shaped dict, or None if no row carries a numeric tx."""
    rng = random.Random(seed)
    samples = []
    for x in synth:
        tx = x.get("tx")
        if tx is None:
            continue
        samples.append(base + tx + rng.gauss(0.0, sigma))
    return cp.rssi_hist(samples)   # None when samples is empty

def d_rssi(synth, profile):
    """Separability of the modeled decoy RSSI shape from the real crowd's, placement-invariant.
    0.0 when the profile has no RSSI (older capture) or the decoys carry no tx (no evidence)."""
    real_bins = profile.get("rssi_bins")
    if not real_bins:
        return 0.0
    decoy = modeled_decoy_rssi_hist(synth)
    if not decoy:
        return 0.0
    return rssi_separability((decoy["rssi_bins"], decoy["rssi_median"]),
                             (real_bins, profile.get("rssi_median", 0.0)))
```

- [ ] **Step 4: Run it to verify it passes**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tests/test_discriminators_rssi.py -v`
Expected: PASS (all six).

- [ ] **Step 5: Commit**

```bash
git add tools/decoy_audit/discriminators.py tools/decoy_audit/tests/test_discriminators_rssi.py
git commit -m "$(cat <<'EOF'
feat(decoy_audit): model single-node decoy RSSI + d_rssi discriminator

Model decoy RSSI host-side as per-identity tx dither + N(0,sigma=4dB) jitter
(single-node worst case, no spatial spread), seeded for determinism. d_rssi
scores it against the real crowd's RSSI via the placement-invariant comparator;
0 when the profile lacks RSSI or the decoys carry no tx.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 4: Wire `rssi_physical` into the scorecard (headline-eligible, modeled)

**Files:**
- Modify: `tools/decoy_audit/scorecard.py` (`build_scorecard`)
- Modify: `tools/decoy_audit/tests/test_scorecard.py` (extend)

**Interfaces:**
- Consumes: `discriminators.d_rssi(synth, profile) -> float` (Task 3).
- Produces: `build_scorecard` appends one row `{"name":"rssi_physical","separability":<float>,"visibility":"modeled"}` when `profile` carries `rssi_bins`; the row is ranked and headline-eligible like every other.

- [ ] **Step 1: Write the failing tests**

Append to `tools/decoy_audit/tests/test_scorecard.py` (inside the `Card` class or a new class — shown as a new class):

```python
class Rssi(unittest.TestCase):
    def setUp(self):
        # decoys tight in tx; real crowd broad in RSSI -> rssi_physical should be a real tell.
        self.synth=[{"atype":"static","itvl_ms":150,"company":0x75,"tx":t}
                    for t in ([-12,-9,-6,-3,0,3]*20)]
        import capture_profile as cp
        broad=cp.rssi_hist(list(range(-90,-40)))
        self.profile={"atype":{"static":1.0,"rpa":0.0,"public":0.0},
                      "itvl_bins":[0,0,1.0,0,0,0,0],"vendor":{str(0x75):1.0},
                      "rssi_bins":broad["rssi_bins"],"rssi_median":broad["rssi_median"]}
    def test_rssi_row_present_and_modeled(self):
        card=SC.build_scorecard(self.synth,self.profile)
        row=next((d for d in card["discriminators"] if d["name"]=="rssi_physical"), None)
        self.assertIsNotNone(row, "rssi_physical row missing")
        self.assertEqual(row["visibility"], "modeled")
        self.assertTrue(0.0 < row["separability"] <= 1.0)
    def test_rssi_absent_without_profile_rssi(self):
        prof={k:v for k,v in self.profile.items() if k not in ("rssi_bins","rssi_median")}
        names=[d["name"] for d in SC.build_scorecard(self.synth,prof)["discriminators"]]
        self.assertNotIn("rssi_physical", names)
    def test_rssi_headline_eligible(self):
        # headline is still the max across all rows, rssi included.
        card=SC.build_scorecard(self.synth,self.profile)
        self.assertEqual(card["headline"], max(d["separability"] for d in card["discriminators"]))
```

- [ ] **Step 2: Run them to verify they fail**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tests/test_scorecard.py::Rssi -v`
Expected: FAIL — `test_rssi_row_present_and_modeled` finds no `rssi_physical` row.

- [ ] **Step 3: Append the row in `build_scorecard`**

In `tools/decoy_audit/scorecard.py`, in `build_scorecard`, after the `presence_duration` append block and **before** `ds=sorted(...)`:

```python
    # Modeled physical (RSSI) tell: decoy RSSI is modeled from tx dither + jitter (the board can't
    # hear its own frames), so it is labeled "modeled". Included in the ranking/headline because
    # the physical layer is real exposure -- an adversary co-located with the decoys sees it.
    if profile.get("rssi_bins"):
        ds.append({"name":"rssi_physical",
                   "separability":round(D.d_rssi(synth, profile),4),
                   "visibility":"modeled"})
```

- [ ] **Step 4: Run the scorecard tests**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tests/test_scorecard.py -v`
Expected: PASS (the new `Rssi` class and the existing `Card` tests).

- [ ] **Step 5: Verify the end-to-end run surfaces the row (no code change)**

Confirm `run.ps1` needs no edit — it prints every `build_scorecard` row. From `tools/decoy_audit/` in a Developer PowerShell (build once if needed):

Run: `powershell -File run.ps1 -Capture ..\..\private\newlong.pcap`
Expected: the scorecard table now includes an `rssi_physical … modeled` line; `HEADLINE` reflects the max including it. (If `newlong.pcap` is absent, use `..\..\private\long.pcap` — it is DLT157 and also carries RSSI.)

Note: `run.ps1` also runs a secondary persona-pop scorecard (for the address-type detail). Because it reuses the same RSSI-bearing profile, it will now also print an `rssi_physical` row — and since personas emit `tx=0` (no dither), it reads high. That is expected and honest (personas are modeled as phones, not tx-dithered decoys), not a bug; do not "fix" it.

- [ ] **Step 6: Commit**

```bash
git add tools/decoy_audit/scorecard.py tools/decoy_audit/tests/test_scorecard.py
git commit -m "$(cat <<'EOF'
feat(decoy_audit): add rssi_physical to the scorecard (headline-eligible)

build_scorecard appends a modeled RSSI tell when the profile carries RSSI, ranked
and headline-eligible like the other tells and labeled visibility=modeled. Wired
here (not in score_all) to preserve score_all's logic-only visibility invariant,
mirroring how presence_duration is added. run.ps1 surfaces it with no change.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Task 5: Document the RSSI discriminator + modeled caveat

**Files:**
- Modify: `tools/decoy_audit/README.md`

**Interfaces:** none (docs only).

- [ ] **Step 1: Update the tell list**

In `tools/decoy_audit/README.md`, find the sentence listing the tells (in the "discriminators.py + scorecard.py" bullet): `Tells: address-type mix, interval distribution, vendor histogram, and **AD structure** ...`. Extend it to include the physical tell, e.g. append after the AD-structure clause:

```
Also scored: **presence/lifespan** (per-address on-air duration) and a modeled
**physical (RSSI)** tell.
```

- [ ] **Step 2: Add an RSSI-discriminator subsection**

Add this subsection near the end of `tools/decoy_audit/README.md` (before the last section):

```markdown
## The physical (RSSI) tell — modeled

An ESP32 cannot hear its own injected frames, so decoy RSSI cannot be captured on
the board. The scorecard instead **models** it: single-node worst case, where all
decoys emit from one antenna, so their RSSI spread is only the generator's
per-identity tx-power dither (`{-12,-9,-6,-3,0,+3}` dBm) plus a fixed-position
multipath jitter (σ = 4 dB). That modeled spread (~6.5 dB) is scored, placement-
invariant (median-centered), against a real crowd's RSSI distribution from an
RSSI-bearing capture (Nordic DLT157 or LE-LL-with-PHDR DLT256).

The row is labeled `visibility=modeled`. It is **headline-eligible**: the physical
layer is real exposure (an adversary co-located with the decoys sees it), and it is
usually the worst tell — the honest single-board ceiling that one radio cannot beat
(only the tx-power dither mitigates it). σ is anchored to a real over-air decoy
capture (the modeled spread sits below the measured 10.1 dB), not invented. For a
real over-air number when a decoy OBSERVE capture is available, use
`analyzers/rssi_audit.py`.
```

- [ ] **Step 3: Commit**

```bash
git add tools/decoy_audit/README.md
git commit -m "$(cat <<'EOF'
docs(decoy_audit): document the modeled RSSI/physical tell

Explain the single-node RSSI model (tx dither + jitter), why it is labeled
modeled and headline-eligible, and that rssi_audit.py remains the real over-air
path.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

### Final verification (after all tasks)

- [ ] **Run the whole host suite**

Run: `"C:/Program Files/Python312/python.exe" -m pytest -q` from `tools/decoy_audit/`
Expected: all green (the prior 86 plus the new DLT256, shared-comparator, modeled-RSSI, and scorecard-RSSI tests).

- [ ] **Sanity-check the real numbers (manual, optional)**

From `tools/decoy_audit/` in a Developer PowerShell:
Run: `powershell -File run.ps1 -Capture ..\..\private\newlong.pcap`
Expected: `rssi_physical` appears as `modeled`; its separability is a plausible physical tell (roughly ≥ the flattering bench 0.31 and below 1.0), and the headline reflects it. Record the measured number in the finishing notes.

- [ ] **Finish the branch**

REQUIRED SUB-SKILL: Use superpowers:finishing-a-development-branch. Verify tests pass, then merge `feat/rssi-physical-tell` into `main` locally with `--no-ff`; **no push**.
