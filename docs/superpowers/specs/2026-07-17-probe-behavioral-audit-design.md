# Probe Behavioral Audit — Design

**Date:** 2026-07-17
**Status:** Approved (design), pending implementation plan
**Roadmap:** Phase 3/4 — Wi-Fi probe realism; extends `tools/probe_audit`

## Goal

Measure how separable the Wi-Fi probe-request decoys (the synthetic "fake phones") are from a real
phone crowd, on **behavioral** axes — not the frame *content* that `probe_audit` already verifies
byte-for-byte. A real over-the-air Kismet capture (`private/Kismetscannew.kismet`, home, fleet
active) showed the decoys are content-faithful but identifiable by behavior: many randomized MACs
sharing a few probe fingerprints, wildcard-only, added to a near-empty room. This tool turns
"an analyst spotted them by eye" into a measured number + a ranked fix list, the same discipline
that `decoy_audit` brought to the BLE side.

## Threat model — what this measures

An adversary who passively watches probe requests (Kismet-class) and looks for the *pattern* of the
fake-phone population, rather than any single frame's IEs. The separators found in the capture:
1. **Density** — too many fake phones for the ambient real-phone count.
2. **Wildcard-only** — the decoys never probe a named SSID; a busy real phone probes its saved nets.
3. **Fingerprint multiplicity** — many rotated MACs pile onto the few archetype fingerprints.

## Reference approach (decided: A, with B-ready inputs)

**A — single mixed capture, relative + conservative.** Model the decoy behavior from the real
`probe_agents.c`; profile a `.kismet` capture as the "ambient" reference; score the decoy profile's
deviation from ambient. Works with the capture in hand. The reference is *pluggable* so a cleaner
capture drops in later (**B** — a decoys-OFF ambient baseline, or a phone-dense capture).

### Honest caveat (must stay visible in the tool's output)

On a **decoy-dominated** capture (home: ~32 fake vs ~9 real) the mixed reference is mostly decoys, so
the *blending* axes (wildcard, multiplicity) **understate** separability — the decoys are compared
partly to themselves. Same conservative bias `decoy_audit` carries on `long.pcap`, worse here because
decoys dominate. Therefore:
- On a decoy-dominated capture, the meaningful output is **density dominance** (decoys as a fraction
  of the crowd) — which is exactly the primary tell and is computed cleanly.
- The blending axes become meaningful on a **phone-dense** or **decoys-off** capture (real phones
  dominate the reference). The tool prints its decoy-dominance ratio alongside the scores so a
  misleadingly-low blending score on a decoy-heavy capture is self-evident.

## Architecture (generator-first, mirrors `decoy_audit`)

Three units, each independently testable.

### 1. Decoy behavioral model — `probe_dump --agents` (extended)

`tools/probe_audit/probe_dump.c` already simulates the real `probe_agents.c` lifecycle over ticks.
Extend its `--agents` mode to emit, per agent *reincarnation* across the window, a record carrying:
`archetype` (index; maps 1:1 to a probe fingerprint since an archetype's IEs are fixed),
`birth_tick`, and `wildcard` (always 1 today — the field exists so a future directed-probe change is
measurable without a tool change). Output is line-based like the existing modes. This is the labeled,
capture-independent decoy side.

From these records the model derives the decoy behavioral profile:
- device count over the window (for density),
- MACs-per-fingerprint distribution (multiplicity),
- wildcard fraction (1.0 today).

### 2. Real reference — `analyzers/kismet_behavior.py` (new)

Parse a `.kismet` SQLite file's `devices` table (each row's `device` column is a per-device JSON
blob). Extract, per device: `probe_fingerprint`, whether it probed any **named** SSID
(`probed_ssid_map` entry with a non-empty `dot11.probedssid.ssid`), whether it is a randomized
(locally-administered) MAC, whether it ever associated (`last_bssid`), and first/last time.

Produce the reference behavioral profile over the population of **randomized, probe-only** clients
(the comparable set — the decoys are randomized probe-only phones):
- total distinct probing devices (for density dominance),
- wildcard fraction,
- MACs-per-fingerprint distribution.

**No real SSID strings, MACs, or coordinates are ever emitted** — only counts, fractions, and
fingerprint *hashes*. The `.kismet` stays in gitignored `private/`.

### 3. Scorecard — `probe_behavior_scorecard.py` (new)

Loads the decoy model (runs `probe_dump --agents`) + the reference profile, scores the three axes,
prints a ranked scorecard with a headline = worst tell, and the decoy-dominance ratio as context.
Same shape/idiom as `tools/decoy_audit/scorecard.py`. A `--gate <x>` regression exit code, matching
the existing tools.

## Behavioral axes (v1)

1. **density_dominance** — `modeled_decoy_device_count / total_distinct_probing_devices_in_capture`,
   in [0,1]: the fraction of the *observed* probing crowd attributable to our decoys. On a mixed
   (decoy-inclusive) capture this is the honest **upper bound** on how much of the crowd is fake
   (the capture total already contains the decoys), and the tool labels it "mixed-reference (upper
   bound)". A decoys-off (B) baseline turns it into a true ratio. High = over-population (the home tell).
2. **wildcard_fraction** — separability of the decoy wildcard fraction (1.0) from the reference's,
   as `|decoy_frac - real_frac|` in [0,1].
3. **fingerprint_multiplicity** — separability of the decoy MACs-per-fingerprint distribution from
   the reference's, bucketed by multiplicity, via a **local** `js_divergence` copy in this tool
   (no cross-tool import — `probe_audit` stays self-contained, matching how it already vendors its
   own helpers).

*(Deferred to v2: birth-rate/temporal uniformity — noisier, needs a defensible model of "intermittent
real arrivals".)*

## Files (all under `tools/probe_audit/`)

- Modify `probe_dump.c` — extend `--agents` to emit `archetype, birth_tick, wildcard`.
- Modify the `probe_dump` build (`Makefile`, `run.ps1`, `README.md`) only if new sources are needed
  (none expected — `probe_agents.c`/`probe_frame.c` already compiled in).
- Create `analyzers/kismet_behavior.py` — `.kismet` → reference behavioral profile.
- Create `probe_behavior_scorecard.py` — model + reference + score + headline + `--gate`.
- Create `tests/test_kismet_behavior.py` — parser on an inline synthetic `.kismet` DB; skip-if-absent
  hook for `private/Kismetscannew.kismet`.
- Create `tests/test_probe_behavior_scorecard.py` — each axis on hand-built inputs; headline = max.

## Testing (pure/host, matching the existing `probe_audit` suite)

- **Decoy model:** `probe_dump --agents` emits the new fields deterministically for a fixed seed;
  many reincarnations over a long window → multiplicity > 1 per archetype; wildcard fraction = 1.0.
- **Kismet parser:** build a tiny synthetic `.kismet` (SQLite with a `devices` table; a handful of
  hand-authored JSON blobs — some named-SSID, some wildcard, some associated, mixed MAC types) and
  assert the extracted counts/fractions/multiplicity. A `@skipUnless` class runs against
  `private/Kismetscannew.kismet` when present and asserts sane ranges only (no fixed values — real
  data drifts).
- **Scorecard:** all-wildcard decoys vs a mixed reference → `wildcard_fraction` tell > 0; a reference
  with the same multiplicity shape as the decoys → `fingerprint_multiplicity` ~0; density dominance =
  the count ratio; headline = max of the axes; `--gate` exits non-zero above threshold.

## Out of scope (future)

- **Directed-probe SSID realism** (the constellation-safe per-persona PNL draw) — a separate piece;
  this audit is the measurement that will gate whether it helps. Design insight banked in
  `private/KISMET-VALIDATION.md`.
- **Wi-Fi population-match** (drive agent count from observed density) — the separate density *fix*;
  this audit measures the problem it solves.
- **Birth-rate/temporal axis** — v2.
- **Raw `.pcapng` parsing** — the `.kismet` device table already carries the computed fingerprints and
  probe maps; raw-frame parsing is unnecessary for these axes.
- **Absolute density** — requires a decoys-off (B) baseline; A reports the mixed-reference upper bound.
