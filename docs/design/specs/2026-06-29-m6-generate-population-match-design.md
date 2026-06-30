# M6 — Generate from Model + Population-Match (design spec)

**Status:** Approved (2026-06-29). Builds on M5 (`rf_model_t` + observe). Board-agnostic; first
hardware-verified on the C5 (Ward), where population-match matters most.

**Milestone goal (handoff):** replace M3/M4's static, hand-weighted template population with a roster
**sampled from the live M5 model** — vendors weighted by observed mix, intervals from per-vendor
distributions, payloads from the joint templates, TX dithered — and size the active set to the
observed population so the synthetic crowd matches the room.

## Scope (decided)

- **Bootstrap from the persisted model.** At boot, generate the roster from the NVS model that
  observe mode built. Adapting to a new room = re-run observe mode (updates NVS) → regenerate at next
  boot. Continuous/automatic re-profiling and concurrent observe+advertise stay in **M8**.
- **Synthesize for any observed company-ID.** Sample company-IDs straight from the observed mix; use
  a specific template when one matches, else build a generic-but-valid vendor manufacturer-data blob
  carrying that exact company-ID. The generated vendor mix then matches the environment.
- **Persona-tuned population-match sizing** (Ward denser than Shade), still anchored to observed
  density.

## Invariants preserved

- **Refined Law 3** — never emit Apple Continuity (`0x07`/`0x0F`) or Find My (`0x12`). Observed Apple
  (`0x004C`) is generated as **iBeacon** (safe subtype `0x02`), regardless of what real Apple ads the
  room contained. The self-test's `has_apple_popup_subtype()` guard runs over the generated roster.
- **Non-connectable** advertising; **aggregates-only** (the model already holds no identifiers, M5);
  **population-match** (invariant 4 — active set never far exceeds observed density).
- **No verbatim replay** — we only know per-vendor *counts* and interval *bins*, never real payloads,
  so generation is always synthetic.

## Generation algorithm (per identity)

1. **Sample a vendor:** pick a `company_id` weighted by observed vendor counts — the 24-slot table +
   `other_count` + the `0xFFFF` no-mfg bucket.
2. **Build the payload:**
   - `0xFFFF` (no-mfg observed) → weighted pick among the beacon/tracker encoders (iBeacon /
     Eddystone-UID / Eddystone-URL / Tile). *Approximation: M5 doesn't record which service-UUIDs
     these were — a future M5 enhancement (svc-UUID histogram) refines this.*
   - `0x004C` (Apple) → **iBeacon** (safe subtype).
   - matches a known template's `company_id` → that template's encoder.
   - otherwise → **`template_build_vendor_mfg(company_id)`** — a generic structured vendor blob
     (company + model + status + battery + plausible bytes) carrying the sampled company-ID.
3. **Sample the interval** from *that vendor's* observed interval histogram: pick a bin weighted by
   its count, then a uniform ms within the bin's `[lo, hi)` (the `>2000` bin caps at ~3000 ms). If
   the vendor has no interval samples, fall back to the chosen template's band.
4. **MAC:** fresh random-static (top two bits set), as today.
5. **TX power:** a per-identity dithered value (see below).

## Population-match sizing (persona-tuned)

```
active_target = clamp( round(pop_ewma * factor), floor, ceiling )
```

| Persona | factor | floor | ceiling | rationale |
|---------|--------|-------|---------|-----------|
| **Shade** (C6, EDC) | 1.1 | 4 | 8 | low-power, conservative; avoid being a traveling cluster |
| **Ward** (C5, car/fixed) | 1.5 | 6 | 16 | denser cover; car power + headroom (room to ~24 later if cadence holds) |

- Persona selected at compile time via `SIMULACRA_PERSONA` (`WARD` / `SHADE`), **defaulting from the
  target** (`CONFIG_IDF_TARGET_ESP32C5` → Ward, else Shade), overridable by `-D`/source.
- `active_target` is a **runtime value** (not the old compile-time `CHURN_ACTIVE_SET` constant) — so
  future milestones (M8 Wi-Fi coexistence) can dial it freely without a rebuild lock-in. The active
  array keeps capacity `CHURN_ACTIVE_SET` (raised to ≥16); churn honors `active_target ≤ capacity`.
- Still **anchored to `pop_ewma`** — a busy room yields a big crowd, an empty one a modest floor.

## TX-power dither

Today all decoys advertise at `tx_power=127` (max) → identical strong RSSI is a tell. Add
`identity_t.tx_power`, set per identity to a dithered value across a plausible spread (loosely shaped
by the observed RSSI histogram), so the crowd shows varied signal strengths. `churn_adv_apply` uses
the identity's `tx_power` instead of the constant.

## Sparse-model fallback

If the loaded model is too sparse to be representative (`total_obs < 50`, tunable) → fall back to the
M4 `templates_pick()` population. Guarantees a believable device on a fresh, never-observed unit.

## Module layout

- **New `generate.{h,c}`** — owns sampling: `generate_roster(const rf_model_t*, identity_t*, size_t)`,
  `generate_active_target(const rf_model_t*)`, weighted-bin helpers, the persona profile table. No
  NimBLE dependency → unit-testable.
- **`roster.c`** — `roster_init` orchestrates: load NVS model → usable? `generate_roster` : existing
  template path.
- **`templates.{h,c}`** — add `template_build_vendor_mfg(uint16_t company_id, uint8_t out[31],
  uint8_t *out_len, uint16_t *out_itvl_ms)` (or split the interval out), reusing `enc_vendor_mfg`.
- **`identity.h`** — add `uint8_t tx_power`.
- **`churn.{h,c}`** — runtime `churn_set_active_target(uint8_t)`; capacity ≥16.
- **`churn_adv.c`** — apply `id->tx_power`.

## Verification

- **On-target self-test** (looped `ST_CHECK`): feed a synthetic model (e.g. 70% `0x0040` / 30%
  `0x0075`, known interval bins, `pop_ewma`), run `generate_roster`, assert:
  - generated vendor distribution ≈ model's mix (within tolerance over 256 draws);
  - intervals fall in the sampled bins; every payload ≤31 B and Law-3-clean;
  - `generate_active_target` matches the persona formula and clamps;
  - **empty/sparse model** → fallback yields a valid template population.
- **Hardware acceptance (C5)**: observe the real room → model in NVS → flash decoy → it generates
  from that model. Confirm via (a) serial dump of the *generated* roster's vendor mix matching the
  observed model, and (b) an nRF scan showing decoys carrying the **room's** company-IDs (e.g. the
  `0x0040`/`0x0075` seen tonight), not the hardcoded M4 set, with a population near `active_target`.
  Optional cap: the two-physical-environments "shift to match" test.

## Out of scope (M6)

Continuous/automatic re-profiling + concurrent observe/advertise (M8); service-UUID distribution
capture (future M5 enhancement); byte-exact proprietary payload cloning; the Wi-Fi dimension (M7).
