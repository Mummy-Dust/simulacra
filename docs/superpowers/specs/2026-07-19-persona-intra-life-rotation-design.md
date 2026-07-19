# Persona Intra-Life Address Rotation — Design

**Date:** 2026-07-19
**Status:** Approved (design), pending implementation plan
**Roadmap:** Phase 4 / M10 — cross-protocol personas follow-up

## Goal

A bound persona rotates **both** its BLE RPA and its Wi-Fi probe MAC **intra-life**, on independent
~8–15 min jittered schedules, while keeping the persona binding intact. Today a bound persona holds a
single address on each radio for its whole life (up to 40 min = `PHANTOM_LIFE_MAX_MS`), letting a
correlator track it far longer than any real phone allows. Real phones rotate the BLE RPA (~15 min,
BT spec default) and the Wi-Fi MAC on independent schedules; this matches that on the fast-realistic end.

## Threat model — what this closes

A passive correlator that links observations by a stable address can follow one of our personas for
up to 40 min. Rotating each radio's address ~every 8–15 min shrinks that window to a real-phone-like
span, closing the presence/lifespan residual (the `decoy_audit` presence axis, ~0.15) for bound
personas. Honest ceiling: rotation cannot beat co-location/RSSI (unchanged); this is the address-layer
of anti-tracking, not the physical layer.

## Design invariants (unchanged by this feature)

- **Binding is by time, not address.** `born_ms` / `life_ms` / `generation` are untouched by
  rotation, so persona co-presence and the cross-protocol linkage are preserved (the cross-protocol
  spec already states rotation within life is realistic and does not break the linkage).
- **Independent per-radio schedules.** The BLE RPA timer and the Wi-Fi MAC timer have independent
  phase + jitter, so the two radios never rotate in lockstep (a real phone doesn't).
- **Law-3.** A bound persona's BLE identity stays an anonymous RPA; rotation changes only the address
  bytes — no vendor/mfg/appearance change, so the Law-3 gate result is unchanged.
- **Uniqueness.** Every rotation draws a fresh address through `uniq_id` (BLE via `make_random_addr`,
  Wi-Fi via `probe_random_mac`); the 2048-entry ring absorbs the extra churn.

## Changes

### 1. BLE RPA intra-life rotation — `main/ble_devices.c`

`ble_devices_tick` currently skips bound devices in the rotation loop (`if (persona_idx >= 0)
continue;` — the "handled below" comment is stale; it is not handled). Replace the skip with a
rotation for bound RPAs:

- When a bound device's `next_rotate_ms` is due, `make_random_addr(d->id.addr, top2_for(BLE_ATYPE_RPA))`
  and reschedule `next_rotate_ms = now_ms + persona_rpa_rotate_base()`; leave `persona_idx`,
  `persona_gen`, `born`/`life`, `atype` (always RPA for bound) untouched.
- New band `PERSONA_RPA_ROT_MIN_MS` / `PERSONA_RPA_ROT_MAX_MS` ≈ **8–15 min** (faster than the unbound
  `RPA_ROT` 10–20 min band). `ble_device_sync` seeds the first `next_rotate_ms` from this band instead
  of the unbound band, so a bound RPA's first rotation is a jittered ~8–15 min out from bind time.

### 2. Wi-Fi probe MAC intra-life rotation — `main/probe_agents.c`

Bound agents do not expire independently (the persona owns their lifetime), so today a bound agent
holds its MAC for the persona life. Add an intra-life MAC-rotation timer:

- `probe_agent_t` gains `uint32_t next_mac_rotate_ms;`.
- Seeded at `probe_agent_sync` (bind) to `born_ms + persona_mac_rotate_base()` (a ~8–15 min jittered
  band; independent draw from the BLE timer).
- A rotation pass (in `probe_agents_lifecycle`, or a dedicated `probe_agents_rotate` called the same
  place) rotates a due agent: `probe_random_mac(a->mac)`, reset the seq base
  (`a->seq = esp_random() & 0x0FFF`), and reschedule — keeping `arch`, `duty`, `born`, `life`,
  `persona_gen`. A rotated MAC is a fresh privacy identity, exactly like a real phone's next probe MAC.

Unbound agents (if any) already churn via reincarnation; the timer is harmless there, but the
rotation only fires on agents whose life exceeds the rotation interval (i.e. the bound, persona-owned
ones), so it targets the 40-min hold without disturbing the fast unbound churn.

## Data flow (per scheduler tick)

1. `phantom_lifecycle` / `phantom_sync_*` — unchanged (persona births/reincarnations, binding).
2. `ble_devices_tick` — bound RPAs now rotate their address when due (BLE timer).
3. `probe_agents` rotation pass — bound agent MACs rotate when due (Wi-Fi timer).

Each rotation is a within-life address change on one radio; the persona's time window and binding are
unchanged, so a scanner sees the persona's BLE (and Wi-Fi) identity "change address" mid-life, exactly
as a real phone does.

## Testing (host, `tools/decoy_audit` + `tools/probe_audit`)

- **BLE rotation (decoy_audit):** a `synth_dump --devices` (or `--personas`) run over a window longer
  than the rotation band shows a bound slot emitting **multiple distinct addresses** at ~8–15 min
  spacing, while its persona binding (index/generation) and time-presence (born→life) are unchanged.
  Assert: ≥2 addresses per bound slot over ~30 min; inter-rotation spacing within band; binding fields
  stable across rotations.
- **Wi-Fi rotation (probe_audit):** a `probe_dump --agents` run shows a bound agent's MAC changing at
  ~8–15 min intra-life (distinct from full reincarnation), seq base reset on each rotation, `arch`
  stable.
- **Presence axis (decoy_audit):** the bound-persona per-address presence distribution shifts from
  up-to-40 min toward ~12 min; confirm the presence/rotation residual does not regress and ideally
  improves.

## Honest ceilings / notes

- **~8–15 min is the fast-realistic end** — below the 15-min BT-spec norm and well under 40, but not so
  fast it becomes its own tell (no real phone rotates every 2–3 min).
- **Does not touch physical/co-location tells** (RSSI/AoA) — this is the address layer only.
- **Slight extra `uniq_id` pressure** from more rotations — negligible against the 2048-entry ring.

## Out of scope

- Changing the persona lifetime band (`PHANTOM_LIFE_*`) — unchanged; only the *address* rotates faster.
- Physical-layer tells (RSSI/fingerprint) — separate axis, unchanged.
- Any change to unbound-device or unbound-agent churn.
