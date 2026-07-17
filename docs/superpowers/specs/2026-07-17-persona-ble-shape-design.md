# Persona BLE-Shape Realism — Design

**Date:** 2026-07-17
**Branch:** `feat/persona-ble-shape`
**Status:** approved (design)

## Problem

A cross-protocol persona binds a Wi-Fi probe identity (phone archetype) to a BLE identity that
shares its lifecycle. Today the BLE twin adopts a **vendor-matched accessory** payload drawn from
the roster library (`ble_device_sync` → `roster_pick_company`):

- Samsung phantom → "Galaxy Buds" (`FMT_VENDOR_MFG` 0x0075, 120–180 ms, name 60%)
- Google phantom → "Pixel Buds" (`FMT_VENDOR_MFG` 0x00E0, 120–180 ms)
- Apple / Generic phantom (`company_id == 0`) → any no-mfg roster shape, which includes **Tile
  trackers, Eddystone beacons, fitness bands**

This produces two tells:

1. **Device-class mismatch.** The Wi-Fi twin behaves like a *phone*; the BLE twin advertises like
   *earbuds / a Tile / a beacon*. A class-aware cross-modal correlator (the SignalTrace threat)
   sees an incoherent device. Real phones do not advertise as their own accessories.
2. **Interval + payload monoculture.** The earbuds templates cluster at 120–180 ms and carry
   0x0075/0x00E0 mfg, so N personas spike the interval and vendor histograms. This is the measured
   `interval +0.12` contribution from the 2026-07-17 persona-atype measurement.

**What real phones emit on BLE (idle/locked, ambient):**
- **iPhone:** Apple Continuity mfg (0x004C) — but Law-3 forbids us (Continuity pop-ups). The honest
  floor is flags-only RPA.
- **Android (Samsung/Pixel/Generic):** mostly flags-only RPA, or a 16-bit service-UUID list. Not
  accessory mfg.

## Goal

Personas draw a **phone-plausible, Law-3-safe BLE shape** — flags-only-dominant with an occasional
16-bit service-UUID list, family-aware, on a widened interval band — instead of the accessory
roster draw. This closes the device-class tell and flattens the interval/vendor spike, while
keeping the persona co-present with its Wi-Fi twin (lifecycle binding is unchanged).

This supersedes the vendor-match added on 2026-07-16 (`roster_pick_company` + the
`test_samsung_google_families_are_vendor_matched` test). The vendor bytes were never the cross-radio
binding mechanism — co-presence and shared lifecycle are — so dropping them costs no coherence.

## Design

### New: `template_build_phone` (templates.c / templates.h)

```c
// Build a phone-plausible BLE advertisement for a persona. Law-3 safe by construction: emits only
// flags-only, or flags + a 16-bit service-UUID LIST (no mfg, no service-data), so it can never
// trigger a Continuity / Fast-Pair pairing pop-up. company_id is always 0. Fills payload/len/itvl.
// apple=true  -> flags-only only (iPhone floor; no Continuity).
// apple=false -> ~60% flags-only, ~40% service-UUID list (Android phone).
int template_build_phone(bool apple, uint8_t out_payload[31], uint8_t *out_len, uint16_t *out_itvl_ms);
```

- Reuses the existing `enc_svc_uuid16` path and the flags default in `template_build`.
- Service-UUID list draws from a phone-plausible set. The existing `SVC_UUIDS16[]`
  (`0x180F, 0x180A, 0x1812, 0x181A, 0xFD6F, 0xFE9F, 0xFD5A, 0xFDCD`) is already phone-appropriate;
  reuse it (no new UUID table).
- **Widened interval band:** `PHONE_ITVL_MIN_MS = 180`, `PHONE_ITVL_MAX_MS = 1000`, jittered per
  call — replaces the 120–180 ms earbuds band so N personas spread across the interval histogram.
- No name, no mfg, no service-data on any persona.

### Changed: `ble_device_sync` (ble_devices.c / ble_devices.h)

- Signature: replace `uint16_t company` with `bool apple`. New:
  ```c
  int ble_device_sync(int slot, int persona_idx, bool apple,
                      uint32_t born_ms, uint32_t life_ms, uint32_t generation);
  ```
- Body: instead of `roster_pick_company(company)`, build the shape directly:
  ```c
  d->id.company_id = 0;
  template_build_phone(apple, d->id.payload, &d->id.payload_len, &d->id.adv_itvl_ms);
  d->id.tx_power = 0;
  ```
  then the existing RPA stamping (atype = RPA, fresh unique addr, phantom-owned born/life,
  next_rotate phase, persona_idx/gen) is unchanged.
- `ble_devices.c` gains `#include "templates.h"`.

### Changed: `phantom_sync_ble` (phantom.c)

- Pass the Apple flag instead of the company id:
  ```c
  ble_device_sync(i, i, ph->family == PF_APPLE, ph->born_ms, ph->life_ms, ph->generation);
  ```
- `phantom_company` stays — the dumps (`probe_dump.c`, `synth_dump.c` persona dumps) use it as a
  **family label** (which vendor the persona *is*), independent of what it broadcasts on BLE. An
  iPhone persona labeled "Apple" that emits no Continuity mfg is coherent, not contradictory.

### Cleanup

- **Remove `roster_pick_company`** (roster.c / roster.h) — no remaining production caller. YAGNI.
- **`tools/probe_audit/host_stubs/ble_devices_stub.c`** — update the stub signature to the new
  `bool apple` form so probe_audit still links.
- **`tools/decoy_audit/README.md:226`** — drop the reservoir-sampled `roster_pick_company`
  description; replace with the phone-shape summary.

### Tests

- **Replace** `test_samsung_google_families_are_vendor_matched` (asserts the removed vendor-match)
  with `test_persona_ble_is_realistic_phone_shape`:
  - every persona BLE row carries `company == 0` (no mfg, and in particular not Apple 0x004C);
  - across a run the interval column shows spread (not a single 120–180 clone band) — asserts
    `len(distinct intervals) > 1`.
- Keep `test_ble_members_are_rpa_and_law3_safe` (atype rpa, comp != Apple) — still valid.
- `test_same_vendor_personas_get_diverse_payloads` keyed on `comp`; with all personas at
  `comp == 0` it collapses to one bucket. Fold its intent (interval diversity) into the new
  realistic-shape test and remove the now-redundant vendor-keyed version.
- Host C tests unaffected (probe_audit uses the stub).

### Verification / measurement

After the code lands, re-run the persona-active scorecard:

```
tools/decoy_audit/run.ps1   # the [persona] line: synth_dump --persona-pop | scorecard --atype-detail
```

Report the new `interval`, `ad_structure`, and `address_type_mix` numbers vs `long.pcap`, and the
delta from the 2026-07-17 baseline (interval +0.12 → expected lower; ad_structure ~unchanged;
atype tilt is a separate tracked follow-up and not a goal of this change).

## Non-goals

- The RPA address-type tilt (personas present as RPA; atype +0.15) — separate tracked follow-up.
- Intra-life RPA rotation — separate tracked follow-up.
- Silent live service-data frames (Find-My-Device / Fast Pair non-discoverable) — deferred: Law-3
  pop-up risk and 2026 anachronism (Exposure Notification) outweigh the marginal realism; the
  flags + UUID-list + wide-interval mix already removes the tells this change targets.
