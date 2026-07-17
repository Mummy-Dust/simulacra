# Persona-Active Address-Type Measurement — Design

**Date:** 2026-07-17
**Status:** Approved (design), pending implementation plan
**Context:** Follow-up from the cross-protocol personas (M10 v1) final review.

## Goal

Replace the *analytic estimate* of the persona-induced BLE address-type tilt (~79% RPA on the C5,
~68% on the C6, vs the ~36% RPA `long.pcap` bench reference we calibrated the 52/36/12 mix against)
with a *measured* separability number, and surface it in the decoy_audit scorecard as a tracked,
interpretable metric.

The personas feature forces every bound BLE member to RPA (phones present on BLE as RPA), so the
on-air BLE crowd tilts RPA-heavy once personas are active. The existing scorecard's
`address_type_mix` discriminator never sees this: it scores the **roster** synth (`synth_dump
<seed> <n>`), which draws `make_random_addr_mixed` at 52/36/12 and has no personas. This design
feeds the **persona-active live BLE population** through the same discriminator.

## Reference decision (settled 2026-07-16)

Measure against the existing `long.pcap` bench reference (a static-heavy, ~36%-RPA environment) AND
print the raw `static/rpa/public` fractions of decoy vs real side-by-side, so the separability
number is interpretable rather than a bare scalar. A high number is expected almost by construction
(persona-heavy crowd vs static-heavy reference); the raw fractions expose that the divergence is
RPA-share-driven and that `long.pcap` is not phone-dense. Acquiring a phone-dense reference, and
adaptive-density tuning, remain separate follow-ups — out of scope here.

## Architecture

A dedicated dump mode emits the persona-active population as NDJSON in the existing roster-synth
shape, so the **existing** scorecard and discriminators score it unchanged. This keeps the
discriminators untouched and measures the *real generated* population (not an analytic model).

### Components

1. **`synth_dump --persona-pop <seed> <nph> <ndev> <ticks> <tick_ms>`** (C, `tools/decoy_audit/synth_dump.c`)
   - Runs the full persona system: `roster_init()`, `phantom_init(nph)`, `ble_devices_init(ndev)`
     (caller passes `ndev` already at/above the floor), `probe_agents_init(nph)`, then the tick loop
     (`phantom_lifecycle` → `phantom_sync_wifi` → `phantom_sync_ble` → `ble_devices_tick`) for `ticks`.
   - After the loop, emits **one NDJSON row per live `ble_device`** (both bound RPA personas and the
     unbound static/NRPA/persistent crowd), in the exact shape the roster synth emits:
     `{"addr","atype","company","itvl_ms","tx","arch","plen","ad"}`, reusing the existing
     `atype_of` / `company_onair` / `ad_types_onair` helpers on `d->id`.
   - This snapshot of the standing live population is the on-air BLE composition an observer sees.

2. **Scorecard scoring (unchanged path):** `scorecard.py persona_pop.ndjson profile.json` produces
   the persona-active `address_type_mix` separability, plus `vendor_histogram` / `interval_distribution`
   / `ad_structure` as free bonus (now persona-active).

3. **`scorecard.py --atype-detail`** (`tools/decoy_audit/scorecard.py`) — an additive flag that, when
   set, prints the decoy vs real `static/rpa/public` fractions alongside the `address_type_mix` line.
   Reads the same `synth_distributions(synth)["atype"]` and `profile["atype"]` the discriminator uses,
   so the printed fractions are exactly what produced the score. Default off (existing output unchanged).

4. **`run.ps1`** (`tools/decoy_audit/run.ps1`) — after the existing roster scorecard, add a persona-pop
   generation (`synth_dump --persona-pop …` → `persona_pop.ndjson` in the gitignored `private/` OutDir)
   and a second scorecard invocation with `--atype-detail`, labeled "persona-active", so the standard
   one-step audit shows both the roster headline and the persona-active atype reality.

### Data flow

`long.pcap` → `capture_profile.py` → `profile.json` (real atype) *(existing)*
→ `synth_dump --persona-pop` → `persona_pop.ndjson`
→ `scorecard.py persona_pop.ndjson profile.json --atype-detail` → separability + raw decoy/real fractions.

## Testing

Host pytest (`tools/decoy_audit/tests/`), deterministic via `srand(seed)`:
- `--persona-pop` output contains **both** RPA rows (personas) **and** non-RPA rows (static/NRPA
  unbound crowd) — proving the dump captures the full population, not just bound or just unbound.
- Feeding the persona-pop NDJSON to `synth_distributions` yields an RPA fraction materially higher
  than the roster baseline (e.g. `> 0.6`), confirming the measurement reflects the persona tilt
  rather than the roster's ~0.36.
- `scorecard --atype-detail` prints both decoy and real `static/rpa/public` fractions (parseable).

## Out of scope (separate follow-ups)

- Phone-dense reference capture (apples-to-apples comparison).
- Adaptive density: tuning the decoy population (persona count / `PHANTOM_BLE_UNBOUND`) to the
  *observed* environment rather than one fixed capture — the real fix if the measured tell is high.
- Intra-life RPA rotation; mesh-distributed personas.
