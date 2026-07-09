# Fleet Revoke UI — Design Spec

**Date:** 2026-07-08
**Status:** Approved (design), pending implementation
**Depends on:** Fleet key provisioning (PR #3, merged `1854881`) — enrollment authority,
`fleet_db` allowlist, key rotation, `enroll_open_window`.

## Goal

Give the Vigil operator an on-screen way to **revoke** a previously-enrolled decoy from the
CYD touch panel, closing the last deferred item of the fleet-key-provisioning feature. Revocation
must actually take a node off the fleet (not just remove a list entry), using the rotation
mechanism the enrollment design already specifies.

## Background

Enrollment gave Vigil an allowlist of decoy identity pubkeys (`fleet_db`, sealed on SD) and a
current fleet transport key at some epoch. Per-node revocation was designed as: **remove the id
from the allowlist, then rotate the fleet key.** Surviving allowlisted decoys re-enroll to the new
key on the reopened OFFER window; the revoked node re-requests but is never granted (it's off the
allowlist), so it keeps only the old key while the fleet moves to the new one — and goes dark.

Everything needed already exists on the backend: `fleet_allow_count()`, `fleet_allow_at(i)`,
`fleet_allow_remove(id)`, `fleet_db_rotate()`, `fleet_db_save()`, `enroll_open_window(now)`, and
`enroll_fp()` for fingerprints. What's missing is only the **UI surface and touch interaction** to
pick a node and trigger the revoke. The revoke *logic* was never exercised because there was no way
to select a node.

## Non-goals

- No wire/protocol changes; no new frame types.
- No changes to the shared renderer (`components/simulacra_radar/radar_render.c`) or to decoy/Ward
  displays. All new UI is Vigil-local in `cyd/main/cyd_main.c`.
- Not adding remote/automated revocation — this is operator-driven, on-panel only.

## UI design

### Surface & rendering

A **Vigil-local modal roster screen**, drawn in `cyd_main.c` with the existing `radar_gfx`
primitives, the same way the enrollment overlay is drawn locally. Because classic-ESP32 DRAM can't
spare a full 240×320×2 (150 KB) framebuffer, the modal renders **band-by-band** into the existing
~19 KB band buffer (`LCD_W×40`): for each 40px band `y0 ∈ {0,40,…,280}`, set up a `radar_gfx_t`
with that `y0`, clear it, draw all modal elements at absolute coordinates (pixels outside the band
are clipped by `radar_gfx_pixel`), and `cyd_flush(y0, 40, band, NULL)`. Text that straddles a band
boundary renders correctly because each band draws its own slice.

Gated on `SIMULACRA_FLEET_PROVISION`.

### Entry & modality

- On the **CONTROL** page, a Vigil-local **FLEET** button is drawn as a local overlay in a reserved
  top strip (`y < 36`). Tapping that strip opens the modal. (Drawn locally, not in the shared
  `draw_control`, so non-Vigil displays are unaffected.)
- While the modal is open it **owns all touch input** — the normal view-cycle, CONTROL-page zones,
  and enrollment long-press gestures are suspended until the modal closes.
- Backlight sleep (30 s idle, existing behavior) closes the modal and returns to the radar view.

### Modal layout

```
  FLEET ROSTER  (N enrolled)
  ---------------------------
  > 3f2a-9c11-4b0e-77d2         <- selected row, highlighted bar
    a19c-0dd4-2c4a-8e60
    7be1-55f0-91aa-30cc
    ...  (window scrolls if N > visible rows)
  [  UP  ][ DOWN ][ REVOKE ]    <- button band
  [           EXIT          ]
```

- **Header:** `FLEET ROSTER  (N enrolled)` where `N = fleet_allow_count()`.
- **Rows:** one per allowlisted id; label = `enroll_fp(fleet_allow_at(i))` (same fingerprint the
  decoy prints on its serial and shows during enrollment, so the operator can cross-check identity).
  The selected row is highlighted with a filled bar. A scroll window handles `N` larger than the
  visible row count; the cursor scrolls the window.
- **Empty state:** if `N == 0`, show `no decoys enrolled` and only the `EXIT` button.
- **Buttons** (large, full-width touch zones for robustness on the coarse panel):
  - `UP` / `DOWN` — move the selection cursor (wrap-around).
  - `REVOKE` — **arm/confirm guard:** first tap arms; the button turns red and reads `CONFIRM?`
    for a 3 s window; a second tap on it within 3 s executes the revoke. Any other tap, or the
    timeout, disarms. This prevents a single mis-tap from re-keying the whole fleet.
  - `EXIT` — close the modal, return to the radar view.

### Revoke action (on confirmed REVOKE)

Let `id = fleet_allow_at(sel)`:

1. `fleet_allow_remove(id)`
2. `fleet_db_rotate()` — new random fleet key, `epoch++`
3. `fleet_db_save()` — persist allowlist + new key to SD
4. `enroll_open_window(now)` — reopen the 30 s OFFER window so surviving decoys auto-re-enroll to
   the new key
5. Auto-close the modal (so the operator sees the radar + enrollment overlay reflect re-enrollment)
6. Log: `enroll: REVOKED <fp> -> rotated to epoch N, window reopened`

The revoked id is no longer on the allowlist, so when it answers the reopened OFFER it is never
granted → it retains only the pre-rotation key → the Vigil now seals telemetry requests under the
new key → the revoked decoy can no longer participate and goes dark within seconds.

## Testing

### On-target selftest (headless-buildable)

`fleet_db_selftest` already covers `fleet_allow_remove` and `fleet_db_rotate` individually. Add a
small **composite revoke** assertion: seed allowlist {a,b}, snapshot key/epoch, run the revoke
sequence on `a` (remove + rotate), then assert: `a` gone, `b` present, `count==1`, `epoch` bumped
by 1, and the key changed. This runs in the existing `FLEET_SELFTEST` CYD build.

### Hardware bench test

With the currently-enrolled C5 decoy (epoch 2) + CYD Vigil:
1. Open CONTROL page → tap FLEET → roster shows the C5's fingerprint (cross-check against the
   decoy's serial print).
2. Select it → REVOKE → CONFIRM? → tap again.
3. Confirm CYD logs `REVOKED … rotated to epoch 3`, `fleet.db` saved, window reopened.
4. Confirm the revoked C5 goes dark: it re-requests but the CYD (0 allowed now, or only survivors)
   never grants it; `status rx` for that decoy stops. Reboot CYD → `fleet.db` loads epoch 3 with the
   reduced allowlist (durability).
5. (Optional, needs a 2nd decoy) revoke one of two → the survivor re-enrolls to epoch 3 and keeps
   streaming while the revoked one goes dark.

## Risks / notes

- Touch-zone calibration on this panel is coarse (`TCAL_*` constants); the big button bands are
  sized generously and are bench-tunable, mirroring the enrollment gesture caveat.
- Revoke rotates the **whole** fleet (brief telemetry gap while survivors re-key). This is the
  intended, spec'd mechanism; the confirm-guard makes the disruption deliberate.
- The revoke logic path was previously unexercised on hardware; this feature is also its first
  real end-to-end test.
