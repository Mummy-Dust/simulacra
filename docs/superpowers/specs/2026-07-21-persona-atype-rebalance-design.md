# Persona RPA Address-Type Rebalance — Design

**Date:** 2026-07-21
**Status:** Approved (design), pending implementation plan
**Roadmap:** Phase 4 / M10 — cross-protocol personas follow-up (address-type tilt)

## Goal

Close the persona-driven `address_type_mix` residual (~0.070, measured via `decoy_audit` on the
persona-active population) by rebalancing the BLE crowd's composition, not by changing per-device
realism. The residual exists because persona-bound BLE members are always RPA (correct — phones
present on BLE as RPA), but they are the **majority** of the crowd (16 personas + 8 unbound = 24 on
C5, personas = 67%), while real ambient BLE is mostly non-phone devices (beacons/wearables/tags:
static/NRPA) with phones a minority. The fix rebalances the crowd's mix, not any single device.

## Threat model

An adversary comparing the decoy population's address-type distribution (static / RPA / NRPA-or-
"public") to a real ambient baseline. Today the decoy skews far more RPA-heavy than real (measured
`[static,rpa,public]` decoy `[0.25,0.67,0.08]` vs real `[0.51,0.36,0.12]`) because persona RPAs
dominate the crowd. This closes that composition gap. Honest ceiling: the reference (`long.pcap`) is
a phone-**sparse** capture, so its 36% RPA is not a hard floor — a genuinely phone-dense real
environment would show more RPA than 36%. This fix targets a realistic minority-phone composition,
not exact parity with one reference capture.

## Changes (two constants, no new modules/interfaces)

### 1. Unbound crowd atype weights — `main/ble_devices.c`

`ATYPE_STATIC_W` / `ATYPE_RPA_W` / `ATYPE_NRPA_W` currently `52` / `36` / `12` (a general "calibrated
fleet mix" comment, applied to the **unbound** BLE-only crowd). The unbound crowd represents
non-phone ambient devices (beacons, wearables, tags) which in reality are almost never RPA — RPA is a
phone/OS-level behavior. Retune to **`75` / `5` / `20`** (static-heavy, still a small live-RPA
sliver so the unbound crowd isn't a monoculture, NRPA raised to match real's ~12% "public"/other
share more closely).

### 2. Unbound pool size — `main/probe.c`

`PHANTOM_BLE_UNBOUND` currently `8` (shared constant, both `esp32c5` and `esp32c6` targets — unlike
`PROBE_PHONES`, which is `#if CONFIG_IDF_TARGET_ESP32C5`). Raise to **`16`**, so the non-phone
unbound crowd is large enough to meaningfully dilute the persona-RPA share instead of being half its
size.

**Effect on both targets** (persona count unchanged; only the unbound side grows):

| Board | Personas | Unbound (old→new) | Total (old→new) | Persona share (old→new) |
|---|---|---|---|---|
| C5 | 16 | 8→16 | 24→32 | 67%→50% |
| C6 | 8  | 8→16 | 16→24 | 50%→33% |

Both boards improve (C6 more than C5) from one shared constant — no per-target tuning needed. `32`
and `24` are both ≤ `BLE_DEVICES_MAX` (32), so no overflow.

## Hardware cost — confirmed zero

- **No new radio load.** `CHURN_HW_INSTANCES = 4` — only 4 extended-advertising instances ever
  transmit at once; `churn_tick` time-slices the *whole* population through those 4 slots regardless
  of pool size. Growing the pool only means each identity gets a very slightly lower on-air duty
  cycle sharing the same 4 slots — it does not add concurrent radio work.
- **No new RAM.** `ble_device_t s_dev[BLE_DEVICES_MAX]` (`BLE_DEVICES_MAX = 32`) is already
  statically allocated in the current firmware; `ble_devices_init(n, ...)` with a larger `n` just
  fills more of an array that already exists.

## Projected effect (C5, expected-value estimate)

Using the audit's own Jensen–Shannon formula on expected fractions (unbound 16 at 75/5/20 →
≈12 static / 0.8 RPA / 3.2 NRPA; aggregate over 32 = personas 16 RPA + unbound):

- Aggregate `[static, rpa, public]`: measured decoy `[0.25, 0.67, 0.08]` → projected `[0.375, 0.525,
  0.10]`, vs real `[0.51, 0.36, 0.12]`.
- `address_type_mix`: measured **≈0.070** → projected **≈0.019** (~72% reduction).

**This is an expected-value projection, not a guarantee.** The actual draw is a random weighted
sample per boot/run; the real measured number will differ somewhat and must be confirmed with
`decoy_audit` once implemented. The direction and rough magnitude (a large reduction, landing in
"small residual" territory) is the claim; the exact decimal is not.

## Testing

Entirely via the **existing** `tools/decoy_audit` measurement surface — no new test infrastructure:

- `synth_dump --persona-pop <seed> <n_personas> <n_devices> <ticks> <tickms>` — already builds the
  real persona-active population (existing tool from the atype-tilt measurement work).
- `scorecard.py --atype-detail` — already prints decoy vs real `static/rpa/public` fractions and the
  `address_type_mix` score.
- Verification is: run the existing pipeline before and after the constant changes, confirm
  `address_type_mix` drops substantially (directionally matching the projection above) and no other
  discriminator regresses (full `decoy_audit` suite stays green — the changes touch shared constants
  read by several tests, e.g. `test_address_type_is_a_realistic_mix`, which asserts *a* realistic mix,
  not the specific old weights — the plan confirms no test hardcodes the old 52/36/12 or 8 values in a
  way that would need updating).

## Out of scope

- Raising `BLE_DEVICES_MAX` above 32 to push personas further into minority territory — a bigger
  change (more identities beyond what's currently allocated) with unverified additional cost; not
  needed given the already-large projected improvement.
- Any change to `PROBE_PHONES` (persona count) — unchanged; only the unbound side grows.
- Any change to the Wi-Fi side, RPA/MAC rotation, or the persistent-device role mix — untouched,
  separate axes already addressed elsewhere.
