# RSSI / Physical-Tell Audit Slice — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the decoy_audit tool measure the physical (RSSI) tell — how separable the decoys' RSSI *shape* is from a real crowd's — closing the tool's last discriminator axis.

**Architecture:** Extract RSSI from the BLE pcap into the profile (`rssi_bins` + stats), capture the decoy side via a bench OBSERVE run, and score placement-invariant (median-centered) RSSI shape separability with a standalone analyzer. The real-crowd reference already exists in `private/long.pcap`.

**Tech Stack:** Python 3 (analyzer + pytest), C / ESP-IDF 5.5 (one default-off `observe.c` log line), PowerShell (bench capture).

## Global Constraints

- Public repo: NEVER commit absolute local paths, the OS username, real hardware MACs, or real SSIDs. `private/` is gitignored — never move captures out of it into tracked paths.
- Commit trailers required on every commit:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`
- Work on a feature branch (`feat/rssi-tell`); do NOT `git push`.
- The `OBSERVE_LOG_RSSI` firmware flag defaults OFF — shipped decoy byte-unchanged.
- Python tests run with `"C:/Program Files/Python312/python.exe" -m pytest` (the system `python` on PATH lacks pytest).
- RSSI binning is defined ONCE in `capture_profile.py` (`RSSI_LO=-100`, `RSSI_HI=-30`, `RSSI_W=5` → 14 bins) and imported by the analyzer. Do not redefine it.
- The analyzer is a **measurement**, not pass/fail: it prints numbers and exits 0 (exit 2 only when RSSI data is absent).

---

### Task 1: RSSI extraction + histogram helper in `capture_profile.py`

Pull RSSI out of the pcap (DLT-aware) and add `rssi_bins`/`rssi_median`/`rssi_stdev`/`n_rssi` to the profile. Backward-compatible: a capture without plausible RSSI omits the keys.

**Files:**
- Modify: `tools/decoy_audit/capture_profile.py`
- Test: `tools/decoy_audit/tests/test_capture_profile_rssi.py` (create)

**Interfaces:**
- Produces:
  - `capture_profile.RSSI_LO = -100`, `RSSI_HI = -30`, `RSSI_W = 5`, `RSSI_NBINS = 14` (module constants)
  - `capture_profile.rssi_hist(values) -> dict | None` — `values` is a list of RSSI ints (may contain `None`); returns `{"rssi_bins":[14 floats summing to 1], "rssi_median":float, "rssi_stdev":float, "n_rssi":int}`, or `None` if no usable values.
  - `capture_profile.parse_adverts(path)` now adds `"rssi": int|None` to each advert dict.
  - `capture_profile.build_profile(adverts)` merges `rssi_hist(...)` keys into the profile when present.

- [ ] **Step 1: Write the failing tests**

Create `tools/decoy_audit/tests/test_capture_profile_rssi.py`:

```python
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import capture_profile as cp  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def test_rssi_hist_basic():
    r = cp.rssi_hist([-64] * 10 + [-70] * 10)
    assert r["n_rssi"] == 20
    assert abs(sum(r["rssi_bins"]) - 1.0) < 1e-9
    assert -71 <= r["rssi_median"] <= -63
    assert r["rssi_stdev"] > 0


def test_rssi_hist_empty_is_none():
    assert cp.rssi_hist([]) is None
    assert cp.rssi_hist([None, None]) is None


def test_rssi_hist_clamps_out_of_range():
    # -120 clamps into bin 0, -10 clamps into the top bin; both counted, hist still sums to 1
    r = cp.rssi_hist([-120, -10, -64])
    assert r["n_rssi"] == 3
    assert abs(sum(r["rssi_bins"]) - 1.0) < 1e-9


def test_parse_nordic_sample_has_rssi():
    prof = cp.build_profile(cp.parse_adverts(os.path.join(HERE, "sample_nordic.pcap")))
    assert "rssi_bins" in prof and prof["n_rssi"] > 0
    assert -110 <= prof["rssi_median"] <= -20


def test_parse_dlt256_sample_omits_rssi():
    # the synthetic DLT256 fixture has an all-zero PHDR -> no plausible RSSI -> key omitted
    prof = cp.build_profile(cp.parse_adverts(os.path.join(HERE, "sample_dlt256.pcap")))
    assert "rssi_bins" not in prof
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests/test_capture_profile_rssi.py -v`
Expected: FAIL — `AttributeError: module 'capture_profile' has no attribute 'rssi_hist'`.

- [ ] **Step 3: Add the RSSI constants + `rssi_hist` helper**

In `tools/decoy_audit/capture_profile.py`, after the `ITVL_LO`/`ITVL_HI` line near the top, add:

```python
# RSSI histogram (dBm). Fixed absolute bins; the analyzer centers on the median for
# placement-invariant comparison. -100..-30 in 5 dB steps = 14 bins.
RSSI_LO = -100; RSSI_HI = -30; RSSI_W = 5
RSSI_NBINS = (RSSI_HI - RSSI_LO) // RSSI_W   # 14

def rssi_hist(values):
    """RSSI list (dBm; may contain None) -> {rssi_bins, rssi_median, rssi_stdev, n_rssi}, or None if empty."""
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    bins = [0] * RSSI_NBINS
    for v in vals:
        idx = int((v - RSSI_LO) // RSSI_W)
        idx = 0 if idx < 0 else RSSI_NBINS - 1 if idx >= RSSI_NBINS else idx
        bins[idx] += 1
    s = sum(bins) or 1
    return {"rssi_bins": [b / s for b in bins],
            "rssi_median": statistics.median(vals),
            "rssi_stdev": statistics.pstdev(vals) if len(vals) > 1 else 0.0,
            "n_rssi": len(vals)}

def _rssi_from(linktype, data, aa_off):
    """Per-record RSSI in dBm, or None if unavailable/implausible."""
    r = None
    if linktype == 157 and aa_off >= 7:            # Nordic BLE: -dBm magnitude, 7 bytes before the AA
        r = -data[aa_off - 7]
    elif linktype == 256 and len(data) >= 2:       # LE LL w/ PHDR: signed Signal_Power at offset 1
        b = data[1]; r = b - 256 if b > 127 else b
    if r is None or not (-110 <= r <= -20):        # sanity gate drops zeros/garbage (synthetic fixtures)
        return None
    return r
```

- [ ] **Step 4: Thread RSSI through `parse_adverts`**

In `tools/decoy_audit/capture_profile.py`, change `parse_adverts` to read the link-type and attach RSSI. Replace:

```python
def parse_adverts(path):
    out=[]
    with open(path,"rb") as f:
        f.read(24)
        while True:
            rh=f.read(16)
            if len(rh)<16: break
            ts_s,ts_u,incl,_=struct.unpack("<IIII",rh); data=f.read(incl)
            if len(data)<incl: break
            off=data.find(AA)
            if off<0 or off+6>len(data): continue
            pdu=data[off+4:]
            if len(pdu)<8: continue
            h0,plen=pdu[0],pdu[1]
            if (h0&0x0F) not in (0,2,6): continue
            body=pdu[2:2+plen]
            if len(body)<6: continue
            adva=body[:6]; ad=body[6:]
            out.append({"ts":ts_s+ts_u/1e6, "addr":adva[::-1].hex(),
                        "atype":_atype(adva[5]), "company":_company(ad),
                        "ad_sig":_ad_types(ad)})
    return out
```

with:

```python
def parse_adverts(path):
    out=[]
    with open(path,"rb") as f:
        gh=f.read(24)
        linktype = struct.unpack("<I", gh[20:24])[0] if len(gh) >= 24 else 0
        while True:
            rh=f.read(16)
            if len(rh)<16: break
            ts_s,ts_u,incl,_=struct.unpack("<IIII",rh); data=f.read(incl)
            if len(data)<incl: break
            off=data.find(AA)
            if off<0 or off+6>len(data): continue
            rssi=_rssi_from(linktype, data, off)
            pdu=data[off+4:]
            if len(pdu)<8: continue
            h0,plen=pdu[0],pdu[1]
            if (h0&0x0F) not in (0,2,6): continue
            body=pdu[2:2+plen]
            if len(body)<6: continue
            adva=body[:6]; ad=body[6:]
            out.append({"ts":ts_s+ts_u/1e6, "addr":adva[::-1].hex(),
                        "atype":_atype(adva[5]), "company":_company(ad),
                        "ad_sig":_ad_types(ad), "rssi":rssi})
    return out
```

- [ ] **Step 5: Merge RSSI into `build_profile`**

In `tools/decoy_audit/capture_profile.py`, `build_profile` ends with `return {...}`. Change that final `return` to build a local dict, merge RSSI, and return it. Replace:

```python
    return {"n_adverts":len(adverts),"n_addrs":len(ts),
            "atype":{k:at[k]/n for k in ("static","rpa","public")},
            "itvl_bins":[b/isum for b in ibins],
            "vendor":{k:v/vtot for k,v in ven.items()},
            "ad_sig":{k:v/adtot for k,v in ads.items()},
            "presence_ms_bins":pbins}
```

with:

```python
    prof = {"n_adverts":len(adverts),"n_addrs":len(ts),
            "atype":{k:at[k]/n for k in ("static","rpa","public")},
            "itvl_bins":[b/isum for b in ibins],
            "vendor":{k:v/vtot for k,v in ven.items()},
            "ad_sig":{k:v/adtot for k,v in ads.items()},
            "presence_ms_bins":pbins}
    rh = rssi_hist([a.get("rssi") for a in adverts])
    if rh:
        prof.update(rh)          # rssi_bins / rssi_median / rssi_stdev / n_rssi (omitted if no RSSI)
    return prof
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests/test_capture_profile_rssi.py -v`
Expected: 5 passed.

- [ ] **Step 7: Verify the existing decoy_audit suite still passes (backward-compat)**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests/ -q`
Expected: all passed (the previously-green 51 + the new 5 = 56). If any prior test broke, the profile-shape change regressed something — stop and fix.

- [ ] **Step 8: Commit**

```bash
git add tools/decoy_audit/capture_profile.py tools/decoy_audit/tests/test_capture_profile_rssi.py
git commit -m "$(printf 'feat(rssi-tell): extract RSSI from the pcap into the audit profile\n\nDLT-aware (Nordic DLT157 / LE-LL DLT256); adds rssi_bins + median +\nstdev + n_rssi. Backward-compatible: captures without plausible RSSI\nomit the keys. The real-crowd reference was in long.pcap all along.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy')"
```

---

### Task 2: Placement-invariant RSSI analyzer + unit tests

Score how separable the decoy RSSI *shape* is from the real crowd's, centered on each median so absolute level doesn't drive the number.

**Files:**
- Create: `tools/decoy_audit/analyzers/rssi_audit.py`
- Test: `tools/decoy_audit/tests/test_rssi_audit.py`

**Interfaces:**
- Consumes: `capture_profile.rssi_hist`, `capture_profile.RSSI_LO`, `capture_profile.RSSI_W`, `capture_profile.RSSI_NBINS` (Task 1); `discriminators.js_divergence(p, q) -> float` (existing; normalizes internally, returns [0,1]).
- Produces:
  - `rssi_audit.load_rssi(profile_dict) -> (bins, median, stdev, n) | None`
  - `rssi_audit.rssi_separability(decoy, real) -> float` in [0,1] (both args the 4-tuple from `load_rssi`)
  - `rssi_audit.read_log_rssi(path) -> list[int]` (parses `rssi=<n>` lines)
  - CLI: `rssi_audit.py --real <profile.json> (--decoy-json <profile.json> | --decoy-log <serial.txt>)`

- [ ] **Step 1: Write the failing tests**

Create `tools/decoy_audit/tests/test_rssi_audit.py`:

```python
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
import capture_profile as cp  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "rssi_audit", os.path.join(ROOT, "analyzers", "rssi_audit.py"))
ra = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ra)


def test_narrow_vs_broad_is_separable():
    decoy = ra.load_rssi(cp.rssi_hist([-64, -63, -65, -64, -66, -64, -63, -65] * 5))  # tight ~2 dB
    real = ra.load_rssi(cp.rssi_hist(list(range(-100, -40))))                          # broad 60 dB
    assert ra.rssi_separability(decoy, real) >= 0.3


def test_placement_invariant():
    shape = [-5, -3, -2, 0, 2, 3, 5]
    real = ra.load_rssi(cp.rssi_hist([-80 + s for s in shape] * 10))     # same shape,
    decoy = ra.load_rssi(cp.rssi_hist([-55 + s for s in shape] * 10))    # shifted 25 dB, both in range
    assert ra.rssi_separability(decoy, real) < 0.1


def test_missing_rssi_returns_none():
    assert ra.load_rssi({"atype": {}}) is None


def test_read_log_rssi_parses_lines():
    p = os.path.join(HERE, "_tmp_obs.log")
    with open(p, "w") as f:
        f.write("W (100) observe: obs rssi=-40 company=0x004c\n")
        f.write("noise line\n")
        f.write("W (110) observe: obs rssi=-72 company=0xffff\n")
    try:
        assert ra.read_log_rssi(p) == [-40, -72]
    finally:
        os.remove(p)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests/test_rssi_audit.py -v`
Expected: FAIL — `FileNotFoundError`/`ModuleNotFoundError` for `analyzers/rssi_audit.py` (not created yet).

- [ ] **Step 3: Write the analyzer**

Create `tools/decoy_audit/analyzers/rssi_audit.py`:

```python
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
from discriminators import js_divergence   # noqa: E402
import capture_profile as cp                # noqa: E402


def load_rssi(prof):
    """profile dict -> (bins, median, stdev, n) or None if it carries no RSSI."""
    if not prof or "rssi_bins" not in prof:
        return None
    return (prof["rssi_bins"], prof.get("rssi_median", 0.0),
            prof.get("rssi_stdev", 0.0), prof.get("n_rssi", 0))


def _median_bin(median):
    idx = int((median - cp.RSSI_LO) // cp.RSSI_W)
    return max(0, min(cp.RSSI_NBINS - 1, idx))


def rssi_separability(decoy, real):
    """JS-divergence of the two median-centered RSSI shapes, in [0, 1]."""
    db, dm = decoy[0], decoy[1]
    rb, rm = real[0], real[1]
    dc, rc = _median_bin(dm), _median_bin(rm)
    dd = {i - dc: w for i, w in enumerate(db)}     # relative-index -> weight, aligned on own median
    rr = {i - rc: w for i, w in enumerate(rb)}
    keys = sorted(set(dd) | set(rr))
    return js_divergence([dd.get(k, 0.0) for k in keys], [rr.get(k, 0.0) for k in keys])


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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests/test_rssi_audit.py -v`
Expected: 4 passed.

- [ ] **Step 5: Commit**

```bash
git add tools/decoy_audit/analyzers/rssi_audit.py tools/decoy_audit/tests/test_rssi_audit.py
git commit -m "$(printf 'feat(rssi-tell): placement-invariant RSSI separability analyzer\n\nCenters each distribution on its median, then JS-divergence of the\nshape (reusing discriminators.js_divergence) + spread ratio. Kept out\nof the generator-first score_all because RSSI is empirical. Accepts a\ndecoy profile.json or an OBSERVE serial log.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy')"
```

---

### Task 3: Default-off `OBSERVE_LOG_RSSI` firmware flag

Let one C5 in OBSERVE mode log per-advert RSSI, so we can capture the decoy side. Default off — shipped decoy unaffected.

**Files:**
- Modify: `main/observe.c`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Produces: build flag `OBSERVE_LOG_RSSI` (default `0`); when `1`, OBSERVE mode prints `obs rssi=<dBm> company=0x<hex>` per advert.

- [ ] **Step 1: Add the default-off flag to `main/observe.c`**

In `main/observe.c`, after the existing `SIMULACRA_LEARN` `#ifndef` block near the top (around line 10-12), add:

```c
#ifndef OBSERVE_LOG_RSSI
#define OBSERVE_LOG_RSSI 0   // 1 = log per-advert RSSI for the decoy_audit physical slice (bench only)
#endif
```

- [ ] **Step 2: Add the log line at the report site**

In `main/observe.c`, find the line (around line 182):

```c
    if (s_report_cb) s_report_cb(d->addr.val, d->rssi, company, hitp);   // M9 tap: raw MAC still live here
```

Immediately AFTER it, add:

```c
#if OBSERVE_LOG_RSSI
    ESP_LOGW(TAG, "obs rssi=%d company=0x%04x", (int)d->rssi, (unsigned)company);   // decoy_audit physical slice
#endif
```

- [ ] **Step 3: Forward the flag in `main/CMakeLists.txt`**

In `main/CMakeLists.txt`, the `foreach(flag ...)` list currently ends with:

```cmake
    PROBE_FIX_CH PROBE_FORCE_SHARED SNIFF_FIXED_CH)
```

Change that final line to add the new flag:

```cmake
    PROBE_FIX_CH PROBE_FORCE_SHARED SNIFF_FIXED_CH
    OBSERVE_LOG_RSSI)
```

- [ ] **Step 4: Verify the shipped default build compiles (flag off)**

Activate the ESP-IDF 5.5 env for esp32c5, then from the repo root:
Run: `idf.py build`
Expected: `Project build complete.` (OBSERVE_LOG_RSSI undefined → 0 → log line compiled out).

- [ ] **Step 5: Verify the OBSERVE + flag build compiles**

Run (clean to avoid a sticky prior config):
`rm -rf build sdkconfig; idf.py set-target esp32c5; idf.py -DSIMULACRA_OBSERVE=1 -DOBSERVE_LOG_RSSI=1 build`
Expected: `Project build complete.`

- [ ] **Step 6: Restore a clean tree**

Run: `rm -rf build sdkconfig`

- [ ] **Step 7: Commit**

```bash
git add main/observe.c main/CMakeLists.txt
git commit -m "$(printf 'feat(rssi-tell): default-off OBSERVE_LOG_RSSI for decoy-side capture\n\nOne gated ESP_LOGW in the OBSERVE report path prints per-advert RSSI so\nthe decoy_audit physical slice can capture the decoy RSSI distribution.\nDefault off (mirrors SNIFF_LOG_FRAMES); shipped decoy byte-unchanged.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy')"
```

---

### Task 4: Bench validation (real end-to-end)

Produce the real-crowd RSSI profile, capture the decoy side on the bench, and run the analyzer for a real number. Restore the board afterward.

**Files:**
- None committed (validation). The real profile + capture live in `private/` (gitignored) or `$TEMP`.

**Interfaces:**
- Consumes: Task 1 (`capture_profile.py`), Task 2 (`rssi_audit.py`), Task 3 (`OBSERVE_LOG_RSSI`), `private/long.pcap`, two+ live decoy boards.

- [ ] **Step 1: Build the real-crowd RSSI profile from `long.pcap`**

Run: `"C:/Program Files/Python312/python.exe" tools/decoy_audit/capture_profile.py private/long.pcap "$TEMP/real_rssi.json"`
Then confirm it has RSSI:
Run: `"C:/Program Files/Python312/python.exe" -c "import json,os; d=json.load(open(os.environ['TEMP']+'/real_rssi.json')); print('n_rssi', d.get('n_rssi'), 'median', d.get('rssi_median'), 'stdev', round(d.get('rssi_stdev',0),1))"`
Expected: `n_rssi ~193000 median ~-64 stdev ~12.0`.

- [ ] **Step 2: Confirm at least two decoy boards are live to be heard**

Run: `powershell -NoProfile -Command "Get-PnpDevice -PresentOnly -Class Ports | Where-Object FriendlyName -match 'CH343' | Select-Object FriendlyName"`
Expected: two C5 ports. Pick ONE as the observer (e.g. COM16); the other C5 + the C6 remain decoys for it to hear. (If the C6 isn't enumerated it can still be advertising — only the observer needs a serial console.)

- [ ] **Step 3: Flash the observer C5 into OBSERVE + RSSI-logging**

Activate the ESP-IDF 5.5 env. Then (substitute the observer port):
`rm -rf build sdkconfig; idf.py set-target esp32c5; idf.py -DSIMULACRA_OBSERVE=1 -DOBSERVE_LOG_RSSI=1 build; idf.py -DSIMULACRA_OBSERVE=1 -DOBSERVE_LOG_RSSI=1 -p COM16 flash`
Expected: `Hash of data verified.`

- [ ] **Step 4: Capture ~45 s of the observer's serial**

Run (PowerShell; substitute the observer port):
```powershell
$log = "$env:TEMP\observe_rssi.log"
Start-Sleep -Seconds 2
$sp = New-Object System.IO.Ports.SerialPort COM16,115200,([System.IO.Ports.Parity]::None),8,([System.IO.Ports.StopBits]::One)
$sp.DtrEnable=$false; $sp.RtsEnable=$false; $sp.ReadTimeout=1000
$sw = [System.IO.StreamWriter]::new($log,$false)
try { $sp.Open(); $end=(Get-Date).AddSeconds(45)
  while ((Get-Date) -lt $end) { try { $sw.WriteLine($sp.ReadLine()) } catch [TimeoutException] {} } }
finally { $sw.Close(); if ($sp.IsOpen){$sp.Close()} }
(Select-String -Path $log -Pattern 'obs rssi=' | Measure-Object).Count
```
Expected: a count > 50 (plenty of adverts heard). If 0, the board didn't reach OBSERVE mode or no decoys are advertising — re-check Step 2/3.

- [ ] **Step 5: Run the analyzer for the real number**

Run: `"C:/Program Files/Python312/python.exe" tools/decoy_audit/analyzers/rssi_audit.py --real "$TEMP/real_rssi.json" --decoy-log "$TEMP/observe_rssi.log"`
Expected: prints decoy/real median+stdev, spread ratio, and `rssi_separability (...)  -> <read>`. Record the number and the read. (Any result is valid data — a low separability means the TX-power dither + bench spread already make the decoys hard to separate on RSSI shape; a high one quantifies the co-location tell.)

- [ ] **Step 6: Restore the observer board to the shipped decoy build**

Run: `rm -rf build sdkconfig` then reflash the decoy firmware (substitute the observer port):
`& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Fleet -Vbat -Port COM16 -Do all -ReadSeconds 6 -Grep 'coexist|decoy alive'`
Expected: boots into `coexist ...` (normal decoy). The fleet is whole again.

- [ ] **Step 7: Record the outcome**

Note the recorded `rssi_separability` + spread ratio in the session and update the relevant memory (`decoy-audit-tool` / the audit's physical slice). No repo commit (validation produced no tracked changes). Optionally save the numbers to a `private/` note.

---

## Notes for the executor

- **Placement invariance is the point.** Absolute RSSI level is deliberately discarded (centering on the median). If a reviewer expects the score to reflect "decoys are louder/closer," that's wrong — that's a deployment choice, not a tell. The score measures *shape* (spread/modality).
- **Don't RSSI-filter the decoy capture.** Selecting decoys by an RSSI threshold would circularly narrow the very spread we measure. Rely on the decoys dominating the bench; residual ambient devices only bias toward "looks more real," making a separable verdict conservative.
- **Firmware "tests" are builds.** Task 3's verification is a clean compile of the default and OBSERVE+flag configs; the behavioral proof is Task 4 on hardware.
- **Do not push.** Commit locally only; merge happens via finishing-a-development-branch.
