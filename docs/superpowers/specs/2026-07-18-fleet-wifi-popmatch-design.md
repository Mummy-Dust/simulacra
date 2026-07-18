# Fleet-Aware Wi-Fi Population-Match — Design

**Date:** 2026-07-18
**Status:** Approved (design), pending implementation plan
**Roadmap:** Phase 3 — completes Wi-Fi population-match for the multi-node fleet

## Goal

Make the Wi-Fi population-match (density-driven probe-agent count) correct when **more than one
decoy** runs. The single-board case is validated (promiscuous coexists, the target adapts), but the
HW test exposed two multi-decoy defects: a new-firmware C6 counted the two old-firmware C5 decoys'
probes as real phones and scaled to the cap.

## Root causes (both confirmed on hardware)

1. **Cross-counting.** `broadcast_fleet_macs` (`esp_now_link.c:204`) packs only `churn_active_at` =
   BLE identity MACs into the `RADAR_TYPE_FLEET_MACS` frame. It does **not** include the Wi-Fi
   `probe_agents` MACs, so a peer's `fleet_mac_excluded` never matches a fleetmate's *probe* source
   → each decoy counts its fleetmates' probes as real phones.
2. **K× over-population.** Even with exclusion, each of K decoys independently sets its target to
   `wifi_obs_target(density)`, so the fleet emits `K × density`. The mesh `fleet_pop` work already
   divides the *init-time* persona count by K, but the reprofile-time `wifi_obs_target` override
   **undoes** that division — so this also restores consistency with the BLE side.

## Changes

### A. Broadcast probe MACs — `main/esp_now_link.c` `broadcast_fleet_macs`

Collect `probe_agents_at(i)->mac` (for `i` in `[0, probe_agents_count())`) into the same MAC array as
the BLE churn identities, then pack/seal/broadcast as today. RX is unchanged — `fleet_note_peer_macs`
is MAC-agnostic, so a peer's probe MACs land in the same exclusion table.

**Frame budget.** `RADAR_FRAME_MAX` = 250. `fleet_macs_pack` payload = `1 + 6·n`. BLE (≤16) + probe
(≤16) = ≤32 MACs = ≤193 B, which after `radar_wire_seal` overhead must stay ≤ 250. The collection
caps the combined count so the *sealed* frame fits (BLE identities first, then probe MACs, up to a
`FLEET_BCAST_MACS_MAX` that the plan sizes against the measured seal overhead — a conservative 32 fits).

### B. Grow the peer table — `main/fleet.h` `FLEET_MAC_CAP`

48 → **96**. Each decoy now tracks the other peers' BLE **and** probe MACs; with 3 decoys a node
holds ~2 × (16 BLE + 16 probe) = ~64 peer MACs, so 96 gives headroom. Cost ~1 KB RAM
(`96 × sizeof(entry)`).

### C. Divide the target by K — `main/coexist.c`

In the reprofile-tick target update (in the coexist task loop), wrap the target in `fleet_pop_share`:

```c
int base = s_wifi_obs_ok ? wifi_obs_target(now) : WIFI_OBS_FALLBACK;
probe_agents_set_target(fleet_pop_share(base), now);
```

Reuses `main/fleet_pop.{c,h}` and the existing `-DSIMULACRA_FLEET_SIZE=K` flag (already forwarded in
`CMakeLists.txt`). Each of K decoys then emits `density/K`, so the fleet total ≈ observed density.

### Deploy

Flash **all three decoys** with the new firmware and `-DSIMULACRA_FLEET_SIZE=3`. All decoys must run
the same firmware for mutual exclusion to work.

## Data flow (per reprofile tick, per decoy)

1. Promiscuous `rx_cb` has been counting randomized probe sources, now excluding *both* BLE and probe
   MACs of fleetmates (via the enriched broadcast) and never hearing its own frames.
2. `base = wifi_obs_target(now)` = this node's read of the real-phone density (fleetmates removed).
3. `probe_agents_set_target(fleet_pop_share(base), now)` = `base / K` agents on this node.

## Testing

- **Host (already covered):** `fleet_pop_share` (clamp/round/floor) and `fleet_macs_pack`/`unpack`
  round-trip are unit-tested; the `fleet` peer-table (`fleet_note_peer_macs`/`fleet_mac_excluded`,
  eviction) is exercised by the existing `churn_selftest` `test_fleet`.
- **New logic is glue + a constant:** the probe-MAC collection loop and the `FLEET_MAC_CAP` bump are
  compile-verified (esp32c6). If a small pure helper is warranted for the collection, the plan may add
  one with a host test; otherwise it stays inline glue.
- **HW-gated (the real proof):** flash all three decoys with `SIMULACRA_FLEET_SIZE=3`, re-capture
  ~30 min with Kismet, and confirm the decoy footprint shrinks (each board scales to ~`density/3`;
  the wildcard-only cluster count drops toward a realistic crowd). Compare against the baseline in
  `private/KISMET-VALIDATION.md`.

## Honest ceilings / notes

- **Static K.** `FLEET_SIZE` is flash-time; adding/removing a node needs a re-flash. A live census
  (`fleet_peer_count`) is a possible future refinement, out of scope here.
- **Clamp-then-divide.** `fleet_pop_share(clamp(D, floor, cap))` differs from divide-then-clamp only
  at `D > 16` (dense environment), where it conservatively under-populates. Fine at home.
- **Floor interaction.** `fleet_pop_share(WIFI_OBS_FLOOR=2)` with K=3 = 1 per node → fleet floor ≈ K.
  A few fake phones across the fleet in a near-empty room, which is acceptable.

## Out of scope

- Live fleet-size census (dynamic K).
- Any change to the RX path or `wifi_density` estimator (unchanged).
- The directed-probe SSID work (separate, audit-gated; banked in `private/KISMET-VALIDATION.md`).
