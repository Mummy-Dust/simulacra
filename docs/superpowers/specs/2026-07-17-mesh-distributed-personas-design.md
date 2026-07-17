# Mesh-Distributed Persona Population (M10 v2) — Design

**Date:** 2026-07-17
**Status:** Approved (design), pending implementation plan
**Roadmap:** Phase 4 / M10 — Cross-protocol personas (mesh v2); relates to M9 (the Coven)

## Goal

Give the decoy **crowd** genuine spatial diversity by distributing *whole* personas — and the whole
decoy population — across `K` physically separated fleet nodes. Each node runs `budget / K` of the
population, so `K` nodes place the crowd at `K` distinct physical points instead of one. This beats
the crowd-level co-location / RSSI tell **proportionally to K**.

Each persona keeps its two radios (BLE + Wi-Fi) co-located **on its node**, exactly as a real
dual-radio phone does. We do **not** split a persona's radios across nodes — that would make one
device's BLE and Wi-Fi originate metres apart, a per-device tell no real phone produces. The tell we
are closing is a property of the *crowd* (all decoys radiating from one antenna), so the fix is
spatial diversity of the crowd, not of the pair.

## Threat model (what this does and does not beat)

- **Beats (partially, ∝ K):** the crowd-level **co-location / RSSI** tell. The detectability audit
  measures a single node's decoy crowd at ~0.15 separable from a real crowd because all N decoys
  share one antenna origin; a real crowd of N devices spans N points. `K` separated nodes give the
  crowd `K` origins, narrowing that gap. The **RF-hardware-fingerprint** tell (M9 Coven's target)
  also improves ∝ K — `K` distinct radios instead of one behind the whole crowd — though this design
  does not *measure* hardware fingerprints (the audit models RSSI/position, not fingerprint).
- **Does not beat (honest ceiling):** `K` nodes give `K` points, not `N`. A real 24-device crowd
  occupies ~24 points; two nodes give two. The win is proportional to node count — it *reduces* the
  clustering, it does not eliminate it. Only more nodes close it further.
- **Additive only:** cannot silence the user's real device. Strongest posture remains Simulacra +
  emission hygiene.

## Design laws (this feature respects all five)

1. **Aggregates only** — no per-device real identifiers stored or replayed. Unchanged.
2. **No verbatim replay** — every identity is freshly synthesized. Unchanged.
3. **Non-connectable / no pairing-popup formats** — unchanged; personas already pass the Law-3 gate
   at construction.
4. **Population-match** — this feature *strengthens* law 4: it prevents `K` co-located nodes from
   emitting `K × density` (over-population). Each node targets `density / K` so the fleet aggregate
   matches observed density.
5. **No raw capture in shipping builds** — unchanged.

## Architecture

The entire firmware change is a `1 / K` divisor applied at the population-sizing chokepoint. No
coordination protocol, no per-node identity, no runtime messaging — `K` is static configuration.

### New module: `main/fleet_pop.{h,c}` — fleet population share (pure, host-testable)

Owns the fleet-size scalar and the share arithmetic. No radio, no timers.

```c
#pragma once
#include <stdint.h>

// Number of decoy nodes sharing this environment. Compile-time (-DSIMULACRA_FLEET_SIZE=K),
// default 1 (standalone). Floored at 1 so the divisor is always safe.
int fleet_pop_size(void);

// This node's share of a fleet-wide population target: round(target / K), floored at 1 so a
// node never drops a whole population class to zero (always >=1 persona, >=1 active decoy).
// K == 1 returns target unchanged (standalone behaviour is byte-identical).
int fleet_pop_share(int target);
```

- `fleet_pop_size()` reads the compile-time `SIMULACRA_FLEET_SIZE` (default 1) and clamps to `>= 1`.
- `fleet_pop_share(target)` = `target <= 0 ? target : max(1, (target + K/2) / K)` (rounded, floored
  at 1). `K == 1` is the identity function.

### Config

`-DSIMULACRA_FLEET_SIZE=K`, **default 1**, forwarded through the existing `CMakeLists.txt`
`-DSIMULACRA_*` mechanism (the same pattern as `SIMULACRA_ESPNOW`, `SIMULACRA_PROBE`, etc.). Each
node is flashed with the fleet's `K`. No per-node index is needed — see Composition. Because the
default is 1 and `fleet_pop_share` is then the identity, every existing single-node build is
unaffected (byte-identical population sizing).

### Applied at the sizing chokepoint (`main/simulacra_main.c`, the combined-coexist decoy block)

Today (≈ lines 141–158) the density-derived population is decided in one place:

- `at = generate_active_target(&m)` → `churn_set_active_target(at)`; `ndev = (int)at` (BLE
  population), floored to `probe_desired_ble_floor()`;
- `phantom_init(probe_phone_target(), …)` → persona count.

The change wraps the fleet-wide targets in `fleet_pop_share()` so each node runs its share:

- the active-set target `at` → `fleet_pop_share(at)` (population-match knob);
- the persona count `probe_phone_target()` → `fleet_pop_share(probe_phone_target())`;
- the BLE population `ndev` and the persona floor `probe_desired_ble_floor()` follow from the scaled
  targets so the BLE-only remainder scales with the personas (the address-type / presence mix we
  already closed must not regress);
- the **fallback path** (no observed model: default `ndev = 12` and the churn default active target)
  is scaled the same way, so a fleet with no learned model still splits its fallback density by `K`.

The exact call sites and the floor interaction are enumerated in the implementation plan.

### Composition stays i.i.d. — no partitioning, no index

Each node independently samples the **same** family / vendor / address-type / interval distributions
it samples today; it just draws fewer identities. The aggregate over `K` nodes therefore reproduces
the single-node distribution. We deliberately do **not** partition the family space per node (e.g.
"node 0 = Samsung, node 1 = Apple") — that would make each node a single-vendor monoculture, a fresh
weak tell, and would require a per-node index. A node needs only the scalar `K`.

### Identity uniqueness across nodes — unchanged

Two nodes could in principle draw the same random address; at 46 random bits the collision
probability is ~2⁻⁴⁶ (effectively never, and never as a *live* duplicate). `fleet_mac_excluded`
(fed by the existing `RADAR_TYPE_FLEET_MACS` broadcast) already stops a node from detecting or
modelling a fleetmate's synthetic MAC. No new uniqueness machinery is required.

## The audit component — quantify the win (`tools/decoy_audit/`)

Extend the modeled RSSI discriminator so the win is a *measured* number, same audit-first discipline
as every other tell.

Today `discriminators.py::modeled_decoy_rssi_hist` models a **single-node** crowd: every decoy's RSSI
is `base + tx_dither_i + N(0, σ=4dB)` from one antenna origin → ~0.15 separable from the real crowd.

Add a **K-node** model:

- Assign each modeled decoy round-robin to one of `K` nodes.
- Each node `n` gets a base position `b_n` drawn from the **real capture's own RSSI distribution**
  (`profile["rssi_bins"]`) — *anchored to reality, no free parameter*. (Rationale: "if our K nodes
  were placed like K random real devices, here is the residual separability.")
- Each decoy's modeled RSSI is `b_{node(i)} + tx_dither_i + N(0, σ)`.
- Score placement-invariant separability (the existing `rssi_separability`) vs the real crowd, as a
  function of `K`.

`K = 1` must reproduce the current single-node number exactly. As `K` grows, the crowd's between-node
spread approaches the real crowd's spatial spread → separability falls monotonically toward 0. Expose
it as a small helper + a scorecard/CLI affordance (e.g. `--fleet-size K`, and/or a printed
separability-vs-K row) so the roadmap can cite the curve. Exact surface defined in the plan.

## Testing (all pure/host, matching existing suites)

**Firmware — `main/fleet_pop.{c,h}` (host unit test, like other pure cores):**
- `K = 1` → `fleet_pop_share(target) == target` for a range of targets (standalone identity).
- `K = 2` → halves with rounding (e.g. `16 → 8`, `15 → 8`, `1 → 1`).
- `K >= target` → floors at 1 (never zero personas/decoys).
- Sum invariant: `K` shares of a target sum to approximately the target (within rounding).
- `fleet_pop_size()` clamps a bad/`0` config to `>= 1`.

**Audit — `tools/decoy_audit/` (Python):**
- `K = 1` K-node model reproduces the existing single-node `d_rssi` value.
- Separability is monotonically non-increasing as `K` increases.
- Node base positions are drawn from the real bins (anchored), not a hardcoded spread.
- Degenerate guards: missing `rssi_bins` → 0 (as today); `K <= 1` path unchanged.

**On-target (functional, with the real 2-node hardware):**
- Flash node A (USB, `SIMULACRA_FLEET_SIZE=2`) and node B (powerbank, `SIMULACRA_FLEET_SIZE=2`);
  confirm each runs ~half the single-node population and Vigil's per-node roster shows the two
  per-node counts summing to ≈ the full single-node crowd.
- Regression: a standalone build (`SIMULACRA_FLEET_SIZE` unset/1) sizes its population identically to
  before (compile-verify + on-target population log unchanged).

The **physical-separation RSSI proof** (capturing from a vantage that hears both nodes and measuring
the widened crowd spread) needs a third capture device and is deferred; it is not required to land
this feature — the modeled audit curve is the headline number and the on-target test proves the
functional split.

## Out of scope (YAGNI / future)

- **Dynamic fleet census** (self-healing `K` via an ESP-NOW membership protocol) — a possible v3 if
  the fleet ever becomes large or mobile. v2 is static config.
- **Per-node index / family partitioning** — rejected (monoculture tell; i.i.d. composition is
  better and needs no index).
- **Splitting a persona's two radios across nodes** — rejected (manufactures a per-device
  radios-metres-apart tell; see Threat model).
- **Identity-uniqueness rework across nodes** — unnecessary (negligible collision + existing
  `fleet_mac` exclusion).
- **Heterogeneous-hardware RF-fingerprint diversity** — that is M9 (the Coven) proper; this feature
  benefits from it (∝ K distinct radios) but does not implement or measure it.
