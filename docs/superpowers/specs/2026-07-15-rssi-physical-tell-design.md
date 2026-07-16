# RSSI / physical-tell audit slice — design

**Date:** 2026-07-15
**Status:** approved, ready for implementation plan

## Problem

The `decoy_audit` tool scores how separable the synthetic decoy population is from a real crowd across
logical/temporal features (address-type, interval, vendor, AD-structure, presence). The last unscored
axis is **physical**: RSSI. Co-located decoys transmit from ~one point, so their observed signal
strength can betray them even when every logical feature is perfect. This slice makes that tell
**measurable**.

## Key findings that shaped the design

1. **Not capture-gated — the reference already exists.** `capture_profile.py` parses a BLE pcap by
   scanning for the advertising Access Address and decoding the PDU after it; the bytes *before* it
   are the link-layer pseudo-header, which is exactly where RSSI lives. The existing real-crowd
   capture `private/long.pcap` (Nordic DLT157) yields **193,363 RSSI samples**: min −100, max −39,
   median −64, **stdev 12 dB**, smooth and unimodal across ~60 dB. The parser simply discarded it.

2. **The tell is spread/shape, not absolute level.** A real crowd spans ~60 dB (many devices, many
   distances, motion). Co-located decoys — even with the existing per-identity TX-power dither
   (`generate.c` dithers `{−12,−9,−6,−3,0,+3}` dBm into `ble_gap_ext_adv_configure`) — are capped at
   the dither range from one point. Absolute RSSI level just encodes *where you placed the boards*
   (a deployment choice), so the discriminator must be **placement-invariant**: center each
   distribution and compare shape.

3. **RSSI is empirical, not generator-derivable.** `synth_dump` emits `tx_power` but not RSSI (the
   generator has no notion of physical placement). Modeling RSSI from `tx_power` needs a free
   multipath-noise parameter — a number that would encode assumptions rather than measured reality.
   So the decoy side comes from a real **capture**, and the analyzer lives outside the generator-first
   `score_all`.

## Design

Four pieces.

### 1. RSSI extraction — `capture_profile.py`

Read the pcap global-header link-type and extract per-advert RSSI, DLT-aware:
- **DLT157 (Nordic):** `rssi = -data[aa_off - 7]` (stored as a positive −dBm magnitude; validated
  against `long.pcap` → mean ≈ −66.8).
- **DLT256 (LE LL w/ PHDR):** `rssi = signed(data[1])` (Signal_Power byte of the 10-byte PHDR).

Histogram into fixed absolute bins **−100…−30 dBm in 5 dB steps** (14 bins; out-of-range clamped to
the end bins). Add to the profile JSON:
- `rssi_bins`: normalized 14-bin histogram
- `rssi_median`, `rssi_stdev`: floats (dBm)
- `n_rssi`: sample count

Backward-compatible: a capture with no usable RSSI (e.g. the synthetic DLT256 test fixture, all-zero
PHDR) omits the RSSI keys, and downstream treats their absence as "no evidence."

### 2. Decoy-side capture — bench OBSERVE

`observe.c` already receives per-advert RSSI (`d->rssi` from EXT_DISC reports) but only aggregates it.
Add a per-advert log line gated by a **committed, default-off** `OBSERVE_LOG_RSSI` flag (mirrors the
existing `SNIFF_LOG_FRAMES` pattern), forwarded via `main/CMakeLists.txt`. Flash one C5 with
`-DSIMULACRA_OBSERVE=1 -DOBSERVE_LOG_RSSI=1`, let it hear the live fleet, capture serial, then
**restore the board to the shipped decoy build** (flag off). The flag stays in the tree so future
decoy captures are one build away; the shipped decoy is unaffected because it defaults off.

- **Log format:** `obs rssi=<dBm> company=0x<hex>` — parseable with a simple regex.
- **Decoy isolation:** capture in a decoy-dominated bench (2 remaining boards × ~8–16 agents each vastly
  outnumber ambient BLE). **No RSSI-based filtering** — filtering on RSSI would circularly narrow the
  spread we're measuring. Residual ambient real devices bias the decoy distribution toward *looking
  more real*, so a "separable" verdict is conservative/robust.
- The observer can't hear itself ([[esp32-no-self-rx]] — same limitation), but the other decoys are
  plenty.

### 3. Physical RSSI analyzer — `analyzers/rssi_audit.py`

Consumes a **real** profile (`rssi_bins` etc.) and a **decoy** RSSI source (a profile JSON, or an
OBSERVE serial log). The histogram helper — `rssi_hist(values) -> {rssi_bins, rssi_median,
rssi_stdev, n_rssi}` and its bin constants — lives in `capture_profile.py` (the profile-format owner);
both `capture_profile.py` and `rssi_audit.py` use it, so binning is defined once. Then:
- **Placement-invariant shape score:** shift each histogram so its median bin aligns to relative index
  0, build vectors over the union of relative indices, and compute **JS-divergence** (reusing
  `discriminators.js_divergence`). Range 0 (indistinguishable) … 1 (trivially separable). This is the
  headline `rssi_separability`.
- **Spread ratio:** `decoy_stdev / real_stdev` — an interpretable diagnostic (≪ 1 = decoys too tight).
- Prints `rssi_separability`, spread ratio, both medians/stdevs, sample counts, and a plain read
  (`indistinguishable` / `marginal` / `separable`).

Kept separate from `score_all` (which stays generator-first) because RSSI requires a physical capture.

### 4. Validation

- **Unit tests** (`tools/decoy_audit/tests/`) with synthetic histograms:
  - narrow-banded decoy vs broad-smooth real → high `rssi_separability`, spread ratio ≪ 1
  - broad vs broad (same shape, shifted level) → ~0 separability (placement invariance holds)
  - missing `rssi_bins` on either side → analyzer reports "no evidence," no crash
- **Real end-to-end:** `capture_profile.py long.pcap` → real profile with `rssi_bins`; bench OBSERVE
  capture → decoy histogram; run `rssi_audit.py` and record the number.

## Data flow

```
private/long.pcap (Nordic DLT157)                 live fleet (2 decoy boards)
  capture_profile.py                                1 C5 in OBSERVE + OBSERVE_LOG_RSSI
  -> rssi_bins, rssi_median, rssi_stdev             -> serial "obs rssi=.. company=.."
        |                                                   |
        | real profile.json                                | decoy serial log
        v                                                   v
                    rssi_audit.py  --real <json>  --decoy-log <serial>
                      center on median -> JS-divergence (shape)
                      spread ratio = decoy_stdev / real_stdev
                      -> rssi_separability [0..1] + plain read
```

## Honest scope / caveats

- The bench number is a **worst-case-ish co-location read** (boards clustered on a desk). Spreading
  boards across a room in deployment widens the decoy RSSI and lowers the tell. The tool measures
  whatever geometry the capture reflects; it does not claim one universal number.
- Absolute RSSI level is deliberately discarded (placement-invariance). We measure *shape*, which is
  the deployment-independent part of the tell.
- This closes the audit tool's last discriminator axis. It is a **measurement**, not a mitigation; the
  existing TX-power dither is the mitigation, and this quantifies how well it works.

## Non-goals (YAGNI)

- **No RSSI modeling from `tx_power`.** Rejected in favor of a real capture (finding 3).
- **No new mitigation** (e.g. RSSI-motion via `tx_power` random-walk) in this slice — measure first;
  only add a mitigation if the number warrants it.
- **No change to the generator-first `score_all`** or the shipped decoy firmware (the OBSERVE log line
  is committed default-off; the board is restored to the shipped, flag-off build after the capture).

## Footprint

- `tools/decoy_audit/capture_profile.py`: RSSI extraction + `rssi_bins`/`rssi_median`/`rssi_stdev`.
- `tools/decoy_audit/analyzers/rssi_audit.py`: new analyzer (reuses `js_divergence`).
- `tools/decoy_audit/tests/test_rssi_audit.py`: unit tests.
- `main/observe.c` + `main/CMakeLists.txt`: one committed, default-off `OBSERVE_LOG_RSSI` log line
  (shipped decoy build leaves it off).
- Docs: a line in the `decoy_audit` README pointing at the physical slice.
