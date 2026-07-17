# RSSI / TX physical-tell audit slice — design

**Date:** 2026-07-17
**Branch:** `feat/rssi-physical-tell`
**Status:** design approved, pending spec review

## Goal

Close the last open axis of the decoy detectability audit: the **physical (RSSI)
tell**. Make it a first-class, repeatable scorecard discriminator that scores how
separable the decoys' RSSI *shape* is from a real crowd's — without requiring a
live over-air capture on every run.

## Background / current state

- `capture_profile.py` already extracts a real-crowd RSSI histogram
  (`rssi_bins`, `rssi_median`, `rssi_stdev`, `n_rssi`) via `_rssi_from`, which
  handles both DLT157 (Nordic) and DLT256 (LE LL with PHDR).
- `analyzers/rssi_audit.py` already implements a **placement-invariant**
  (median-centered) `rssi_separability()` and produced a one-off reading on
  2026-07-16: real crowd stdev 12.0 dB, decoy stdev 10.1 dB (from a 3-board bench
  OBSERVE capture, ~520 samples), **separability 0.31 ("marginal")**. That path
  is hardware-gated (needs a board in OBSERVE mode) and is **not** in the scorecard.
- The main scorecard (`scorecard.py` + `discriminators.py`) has **no** RSSI slice,
  because the host `synth_dump` path has no over-air RSSI: an ESP32 cannot hear its
  own injected frames (hardware fact), so decoy RSSI cannot be captured on-board.
- The decoy generator already dithers transmit power per identity:
  `dither_tx()` picks uniformly from `{-12,-9,-6,-3,0,+3}` dBm (stdev ≈ 5.1 dB),
  and `synth_dump` emits it as the `tx` field of each NDJSON row.

Two problems block the slice: (1) `capture_profile.py` silently mis-parses DLT256,
so an RSSI-bearing capture like `newlong.pcap` yields 0 adverts; (2) there is no
host-repeatable decoy RSSI to score.

## Global constraints (verbatim)

- Public repo `github.com/Mummy-Dust/simulacra`. Commit identity **must** be
  `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>`.
- **Never commit** the OS username, absolute `C:\Users\<name>` paths, real MACs,
  real SSIDs, or `private/` scratch (captures live in the gitignored `private/`).
- Every commit carries the trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.
- Merge to `main` locally with `--no-ff`; **no push**.
- Host build is MSVC `cl` (no gcc/clang on this host); tests run with
  `C:/Program Files/Python312/python.exe -m pytest`. Firmware is untouched.

## Non-goals

- No firmware changes (the tx-power dither is the existing mitigation; this only
  *measures* it).
- No change to `analyzers/rssi_audit.py`'s real-over-air behavior — it stays as the
  ground-truth tool for when a live decoy capture is available.
- Not a fresh over-air measurement each run — the scorecard slice is explicitly a
  **model**, labeled as such.

## Piece 1 — DLT256 parse fix

`parse_adverts` locates each advert with `data.find(AA)` (AA = `d6 be 89 8e`). In
DLT256 (LE LL **with PHDR**) the 10-byte pseudo-header carries a *reference* copy of
the Access Address at record offset 4, so `find` stops there instead of the real
packet AA at offset 10. Every PDU is then read 6 bytes early and discarded → 0
adverts. (Verified against the real bytes of `newlong.pcap`: reference AA at
offset 4, real packet AA at offset 10, PDU header at offset 14.)

**Fix:** when `linktype == 256`, search past the PHDR — `data.find(AA, 8)` — so the
match lands on the real packet AA at offset 10. DLT157 keeps `find(AA)` from offset
0 (that framing has a single AA and already works). `_rssi_from` is already correct
for both link types, so per-record RSSI populates for free once the offset is right.

This is the one place the tool's "DLT256 aware" docstring claim was never exercised
(the older `long.pcap` is DLT157).

## Piece 2 — the RSSI model (single-node worst case)

For each decoy identity `i` in the `synth_dump` NDJSON:

```
decoy_rssi_i  =  tx_power_i  +  jitter_i,     jitter_i ~ N(0, σ²)
```

- **Geometry = single node.** All decoys emit from one antenna at one point, so
  there is **no** spatial spread — the honest worst case for a one-board deployment
  and the "one radio can't forge N fingerprints" ceiling the roadmap names. Absolute
  path loss is irrelevant because the comparison is median-centered.
- **`tx_power_i`** = the real per-identity dither `synth_dump` already emits (`tx`).
- **`σ` = fixed-position multipath jitter**, the single physical unknown. Set to a
  small indoor fixed-link value, **σ = 4 dB**, and *anchored* to the real capture:
  the model's decoy stdev √(5.1² + 4²) ≈ **6.5 dB** must sit *below* the measured
  over-air 10.1 dB — which it does, because that bench figure also included
  board-spread and ambient-BLE bleed that a single node does not have. So the real
  capture is used as an **upper-bound sanity ceiling**, not to inflate σ (which
  would flatter the decoys). The jitter also de-quantizes the three spiky tx bins so
  separability is not *overstated* by 5-dB-bin quantization alone.
- The modeled histogram is built deterministically (seeded jitter) into the same
  absolute `RSSI_LO/RSSI_W/RSSI_NBINS` bins `capture_profile` uses, then scored with
  the existing placement-invariant `rssi_separability()`.

**Expected result:** more separable than the flattering bench 0.31 (spread ratio
~0.54 vs 0.85). That higher number *is* the honest single-board physical ceiling.

## Piece 3 — scorecard integration

- **Share the comparator.** Move `rssi_separability()` and `_median_bin()` from
  `analyzers/rssi_audit.py` into `discriminators.py`; `rssi_audit.py` imports them
  from there. No behavior change to either — pure DRY.
- `synth_distributions()` gains a modeled decoy RSSI histogram built from the `tx`
  field + seeded N(0, σ) jitter.
- New `d_rssi(sd, prof)`: returns `rssi_separability(decoy_hist, real)` when the
  profile carries `rssi_bins`, else `0.0` (no-evidence, the same pattern as
  `ad_structure` and `presence_duration` for older profiles).
- The scorecard prints one new row: `rssi_physical … visibility=modeled`.
- **RSSI participates in the HEADLINE (max) tell.** RSSI is expected to be the worst
  tell (~0.4+), so the headline will rise from the current structural ~0.17 to
  reflect the physical ceiling. This is deliberate and honest: the audit's premise is
  "an adversary uses their single best discriminator," and the physical layer is real
  exposure. The `visibility=modeled` label and the per-row VISIBILITY column keep its
  provenance unambiguous. (Excluding it would flatter the headline — rejected.)
- The default `--gate` (1.1) is unchanged, so nothing new trips CI; gate policy for a
  modeled tell is left to the operator.

## Data flow

```
newlong.pcap ──capture_profile.py (DLT256 fixed)──▶ profile.json {rssi_bins,...}
synth_dump ──▶ synth.ndjson {tx,...} ──synth_distributions(model σ)──▶ decoy_hist
                                            │
profile.rssi_bins ──────────────────────┐  ▼
                                    d_rssi = rssi_separability(decoy_hist, real)
                                            │
                                            ▼
                              scorecard row "rssi_physical" (modeled) ──▶ headline
```

## Testing

- **DLT256 fix (integration, skips if capture absent):** run `capture_profile` on
  `private/newlong.pcap`; assert `n_adverts > 0`, `n_rssi > 0`, and RSSI stdev in a
  sane band (5–20 dB). Skips cleanly when the gitignored capture is not present, so
  CI without it still passes.
- **RSSI model (unit):** a synthetic `tx` list → modeled histogram has the expected
  stdev (~6.5 dB for the real dither set + σ = 4) and `d_rssi` returns a score in
  (0, 1); a zero-jitter degenerate case stays bounded and finite.
- **Anchor sanity (unit):** modeled single-node separability against a fixed
  reference is `≥` the real bench 0.31 (worst-case is never *less* exposed than the
  spread bench) and `≤ 1.0`.
- The existing suite (86 tests) stays green; `analyzers/rssi_audit.py` behavior is
  unchanged (it imports the moved comparator). `run.ps1` gains an `rssi_physical`
  line in its scorecard output.

## Honest caveats (documented in the tool)

- The scorecard RSSI number is **modeled**, not a fresh over-air reading. Its value
  is a repeatable regression gate on the tx-power dither logic (remove the dither →
  spread collapses → the tell spikes), anchored once to a real capture.
- σ is a single fixed constant, not per-environment. It is deliberately conservative
  (single-node, small jitter). A real multi-node deployment (the Coven) would spread
  the decoys spatially and *lower* this tell — so the modeled number is a ceiling for
  the single-board case, not a promise about a mesh.
- `analyzers/rssi_audit.py` remains the way to get a real over-air number when a
  decoy OBSERVE capture is available; the model does not replace it.

## Files touched

- `tools/decoy_audit/capture_profile.py` — DLT256 AA-offset fix.
- `tools/decoy_audit/discriminators.py` — moved `rssi_separability`/`_median_bin`,
  modeled decoy RSSI in `synth_distributions`, new `d_rssi`, add to `DISCRIMINATORS`.
- `tools/decoy_audit/scorecard.py` — print the `rssi_physical` row (headline-eligible).
- `tools/decoy_audit/analyzers/rssi_audit.py` — import the shared comparator (no
  behavior change).
- `tools/decoy_audit/run.ps1` — add the `rssi_physical` scorecard line.
- `tools/decoy_audit/tests/` — new tests per the Testing section.
- `tools/decoy_audit/README.md` — document the RSSI discriminator + its modeled caveat.
