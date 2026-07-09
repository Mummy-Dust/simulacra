# Decoy Detectability Audit — Design

**Date:** 2026-07-09
**Status:** Approved (brainstorm) — pending implementation plan
**Component:** `tools/decoy_audit/` (new host-side tool)

## Problem

Simulacra's core thesis is that a churn crowd of synthetic BLE decoys poisons
co-travel correlation (the counter to passive fingerprinting / ALPR-style
profilers). That thesis collapses the moment a profiler can cheaply tell a
synthetic decoy from a real device and filter it out. Today we have **no way to
measure how detectable our decoys actually are** — realism work would be guessing.

A 6.1-hour bench capture (real ambient devices **with a decoy running**, nRF
Sniffer / Nordic DLT157) was analyzed as a labeled-ish test set. Finding: simple
single-feature discriminators (address format, RSSI, payload shape) do **not**
cleanly separate the decoys from the real crowd — the decoys blend into the
static-random address population and the broad ambient RSSI range. That is good
for realism, but it also means:

1. We cannot improve realism without a **measurement** that quantifies
   separability and ranks the specific tells worth fixing.
2. We cannot rely on a mixed real-world capture as ground truth, because we
   cannot reliably label which advertisers were ours.

## Goals

- Produce a repeatable **detectability scorecard**: for each discriminator, how
  separable are our decoys from a real reference crowd, calibrated so a
  discriminator does not fire on genuinely-real devices (low false positive).
- Yield a **single headline detectability score** usable as a regression gate,
  so realism work can be tracked as the number drops.
- Rank the concrete tells (address-type mix, interval distribution, vendor
  histogram, TX/RSSI spread, lifespan cohort) to prioritize future realism work.
- Require **no hardware** for the primary (logic-visible) path; run
  deterministically in CI; reuse the established host-tool build pattern.

## Non-Goals

- Not fixing realism tells in this project — this tool *measures*; fixes are
  separate follow-on work it prioritizes.
- Not a live on-device detector. A capture-based (physical-only) path is
  designed for, but landing it is deferred to a later slice.
- Not identity extraction. The tool models **distributions**, never retains or
  emits bystander device identities.

## Approach (chosen)

**Generator-first (host).** Generate the decoy population *ourselves* on the host
so every synthetic advert is a known-decoy label for free, then score how
separable it is from a real reference crowd derived from a capture. A
capture-based path for physical-only tells drops into the same scoring core
later without rework.

Rejected alternatives:
- *Capture-only (live decoy sniff)*: needs a hardware capture each run, is
  non-deterministic, not CI-friendly, and — critically — a **mixed** capture
  cannot be reliably auto-labeled (the whole reason this tool is needed).
- *Improve one tell blind (e.g. just interval realism)*: cheaper, but without
  measurement we cannot tell whether it helped or which tell mattered most.

## Architecture

Three units with clear interfaces, each testable in isolation.

### 1. `real_profile` — reference distribution model
Input: a BLE capture (auto-detect DLT256 and Nordic DLT157; locate the PDU by
scanning for advertising Access Address `0x8E89BED6` so pseudo-header length is
irrelevant). Output: a **distribution model** of "what real looks like":

- address-type shares (static-random / RPA / public-nrpa), read from the AdvA
  MSB top-2-bits — **not** the PDU TxAdd bit (unreliable in the Nordic framing);
- advertising-interval distribution (per-address median gap, 5 ms..60 s window),
  kept as a histogram — real is broad and heavy-tailed;
- RSSI distribution, overall and per-address spread (physical-only signal);
- vendor/company histogram and service-UUID histogram;
- PDU-type mix; device-lifespan cohort histogram (span buckets).

Privacy: retains only aggregate distributions; no addresses, names, or raw AD.

### 2. `synth_source` — labeled decoy population
Compiles the real firmware generation path (`main/generate.c`, `main/roster.c`,
`main/templates.c`, `main/rf_model.*`, `main/learn.c`, plus
`components/simulacra_radar/{law3,learn_wire}.c`) against the existing
`tools/pcap_learn/host_stubs/`. Runs `generate_roster()` against a supplied
`rf_model` and emits the synthetic population — address, AD payload, interval,
TX-power dither, archetype — all labeled decoy. The `rf_model` may be seeded from
a `real_profile` so the comparison is apples-to-apples (same environment shape).

### 3. `discriminators` + `scorecard`
A registry of discriminators, each: (a) tagged `logic-visible` (scoreable from
`synth_source` alone) or `physical-only` (needs a capture in `real_profile`);
(b) returning a **separability score** — how well it distinguishes the synth
population from the real reference, calibrated so it does not fire on the real
devices. Aggregate into a ranked scorecard + one headline score.

The **headline score is the maximum separability across discriminators** (the
single most-detectable tell), not an average — a real adversary picks their best
discriminator, so the worst tell defines our exposure. The scorecard lists all
discriminators ranked by separability so the fix-order is explicit.

Calibration method (mirrors `sig_scan`'s AirTag-selectivity check): tune each
discriminator's threshold against the real reference so its false-positive rate
on genuinely-real devices stays under a fixed budget, then measure its
true-positive rate on the labeled synth population. Separability is reported as
that TPR at the pinned FPR (plus a divergence measure for distributional ones).

## Discriminators (v1)

Grounded in what the bench capture already exposed:

| Discriminator | Signal | Visibility |
|---|---|---|
| Address-type mix | decoys 100% static-random; real crowd ~43% static / 30% RPA / 10% public — decoys never present RPA | logic |
| Interval-distribution shape | real broad/heavy-tailed (119 ms–17 s); generator fallback narrow `100–300 ms` | logic |
| Vendor-histogram divergence | template company set vs. real (real dominated by a non-template company id) | logic |
| Lifespan cohort | churn windows cluster decoy lifespans (~1–10 min); real is bimodal (persistent + transient) | logic |
| TX/RSSI realism | ±12 dB dither & single-emitter clustering vs. real ~50 dB spread | physical |

New discriminators register against a stable interface so the set can grow.

## Ground truth

The generator-first path is self-labeling — no external labels needed for the
primary scorecard. For the deferred capture-based (physical-only) validation, a
clean label source is required and will be specified then; the leading option is
a **decoy-only capture** (decoy running, real devices minimized) or a firmware
identity log emitted during a capture window. This is explicitly out of scope
for the first slice.

## Deliverables

- `tools/decoy_audit/` host tool with the three units above, built via a
  Makefile plus a documented MSVC `cl /FI` shim invocation (matching
  `tools/pcap_learn`; MSVC-only host is the reality here).
- A DLT-auto-detecting capture reader (scan-for-AA), added here rather than by
  editing the DLT256-specific `tools/pcap_learn/parse_pcap.py`.
- A one-page scorecard report (per-discriminator separability + headline score).
- A machine-readable scorecard output suitable as a regression gate.
- A README documenting build, run, the scoring/calibration method, and the
  privacy stance.

## Testing

- Unit: each discriminator against small synthetic fixtures with known
  separability (a deliberately-bad generator config scores high; a
  realistic one scores low).
- Calibration: assert every discriminator's false-positive rate on a real
  reference fixture stays under the pinned budget.
- Reader: fixtures for both DLT256 and Nordic DLT157 framings decode to the
  same advert fields.
- Determinism: fixed RNG seed for `synth_source` so the headline score is
  reproducible in CI.

## Privacy

- Captures and any parsed intermediates stay local and uncommitted (the repo is
  public). `real_profile` retains only aggregate distributions, never device
  identities, names, or raw AD payloads.
- No absolute local paths or usernames in any committed artifact.

## Bonus (noted, not in scope)

The `real_profile` interval/vendor distributions are a privacy-safe,
structure-only enrichment source for the generator itself (whole-population
shape, extending the existing per-shape `learn.seed` interval enrichment). A
natural follow-on once the audit tool quantifies the gap.
