# Dynamic Fleet-Size Census (v1: Wi-Fi target only) — Design

**Date:** 2026-07-22
**Status:** Approved (self-directed — see note below), pending implementation plan
**Roadmap:** Phase 3/4 — robustness follow-up to mesh personas + Wi-Fi population-match

**Process note:** written during an autonomous session — the user explicitly authorized working
"top to bottom... make choices based on what's best for the project" while offline overnight. This
spec follows the normal brainstorming discipline, but the questions a live session would put to the
user are instead resolved here with documented reasoning, so the choices are inspectable and
reversible on review. No code was written before this design was recorded.

## Goal

Replace the static `-DSIMULACRA_FLEET_SIZE=K` divisor (for the **Wi-Fi population-match target
only** — see Scope) with a live census of currently-heard fleet peers, so the fleet self-configures
instead of requiring a re-flash whenever a node joins, leaves, or is temporarily off. Direct
motivation: this session hit the static-K friction twice (manually re-flashing 3 boards with
`-DSIMULACRA_FLEET_SIZE=3` for the fleet-aware Wi-Fi popmatch validation, then again considering a
revert). A live census removes that step going forward.

## Scope decision (self-resolved)

**In scope: only the Wi-Fi coexist reprofile-tick target** (`main/coexist.c`, the
`probe_agents_set_target(fleet_pop_share(wt), now)` call). This site already runs periodically
(~5 min) with a `now_ms` in scope — a live, TTL-based census slots in with zero new periodic-task
machinery.

**Out of scope: BLE/mesh-persona population at boot** (`main/simulacra_main.c`,
`phantom_init(fleet_pop_share(probe_phone_target()), ...)`). This is a **one-time, boot-time**
call that runs *before* `esp_now_link_start()` — at that point in boot the node has not yet had a
chance to hear from any peer, so there is no live census available to consult (a chicken-and-egg
problem: sizing the initial crowd requires knowing K, but K is only knowable after the ESP-NOW link
is up and has heard peers, which happens *after* init). Making the BLE/persona population
live-resizable after boot would need a new `phantom_set_target`-style mechanism (mirroring
`probe_agents_set_target`) plus a re-sync pass through `phantom_sync_ble`/`phantom_sync_wifi` — a
materially bigger change. `-DSIMULACRA_FLEET_SIZE` continues to size the BLE/persona pool at boot,
unchanged. This is a deliberate v1 boundary, not an oversight — flagged plainly so it isn't confused
with a defect if someone changes a node's real fleet membership and only the Wi-Fi side adapts.

## Design

### New peer-node census — `main/fleet.{h,c}`

Fleet nodes already broadcast `RADAR_TYPE_FLEET_MACS` every 25 s (`main/esp_now_link.c:274`), and
`on_recv` already receives `const esp_now_recv_info_t *info` for every frame, which carries the
sender's real hardware MAC (`info->src_addr`) — the same primitive the CYD's `node_id_for()`
(`cyd/main/cyd_main.c:151`) already uses to assign stable per-sender node ids. Decoys don't yet use
this for anything; today `fleet.c` only tracks the *content* of `FLEET_MACS` frames (peer synthetic
MAC exclusion), not the *identity of the sender*.

Add a small, separate table (distinct from the MAC-exclusion table — different purpose, different
key):

```c
#ifndef FLEET_NODE_CAP
#define FLEET_NODE_CAP 8       // distinct peer nodes tracked (real hardware MACs, not synthetic)
#endif

// Note a live peer node (its real ESP-NOW hardware MAC), refreshing if already known.
void   fleet_note_peer_node(const uint8_t mac[6], uint32_t now_ms);
// Distinct non-expired peer nodes heard from recently (self-exclusive -- does not count this node).
size_t fleet_node_count(uint32_t now_ms);
```

Same reuse/eviction/TTL shape as the existing `fleet_note_peer_macs`/`fleet_peer_count`, reusing the
existing `FLEET_MAC_TTL_MS` (90 s = 3.6× the 25 s broadcast period — already the right margin,
no new constant needed).

**Self-loopback is a non-issue.** ESP-NOW, like the promiscuous-RX path already relied on for
`wifi_observe` (`esp32-no-self-rx`), does not deliver a station's own over-the-air transmission back
to its own receive callback — a radio cannot receive its own simultaneous transmission. No explicit
self-MAC check is needed.

**Wiring (`main/esp_now_link.c`, the existing `RADAR_TYPE_FLEET_MACS` handler, ~line 177):** add one
call, `fleet_note_peer_node(info->src_addr, now)`, alongside the existing
`fleet_note_peer_macs(...)` call. No new frame type, no new wire format.

### Live fleet size — `main/fleet_pop.{h,c}`

```c
// Live fleet size = distinct peer nodes heard from recently, + this node. Falls back to 1
// (standalone) when no peers have been heard -- the correct, safe default, achieved for free
// (fleet_node_count returns 0 when nothing has been heard yet, e.g. right after boot).
int fleet_pop_live_size(uint32_t now_ms);
```

Implementation: `return (int)fleet_node_count(now_ms) + 1;` (self). No new file — this is a thin
wrapper alongside the existing `fleet_pop_size()`/`fleet_pop_share()`/`fleet_pop_share_k()`; `main/fleet_pop.c`
already depends conceptually on fleet size, so it's the natural home. (It will need `#include
"fleet.h"` for `fleet_node_count`, which `fleet_pop.c` does not currently include — the existing pure
`fleet_pop_share_k(target, k)` stays exactly as-is, just called with a different `k` at one call site.)

### Wiring the live size into the Wi-Fi popmatch tick — `main/coexist.c`

Replace, at the existing reprofile-tick target line:

```c
probe_agents_set_target(fleet_pop_share(wt), now);
```

with:

```c
probe_agents_set_target(fleet_pop_share_k(wt, fleet_pop_live_size(now)), now);
```

`fleet_pop_share(wt)` (the static-K path, `-DSIMULACRA_FLEET_SIZE`) remains used, unchanged, at the
BLE/persona boot-time site in `simulacra_main.c` — this is the one call site that changes.

## Data flow (per Wi-Fi reprofile tick, ~5 min)

1. Over the preceding ~5 min, `on_recv` has been calling `fleet_note_peer_node(info->src_addr, now)`
   on every `FLEET_MACS` broadcast heard from live peers (every 25 s per peer).
2. `fleet_pop_live_size(now)` = distinct non-expired peer nodes + 1.
3. `fleet_pop_share_k(wt, live_size)` divides the observed Wi-Fi density target by the live fleet
   size instead of the compile-time constant.

**Degrades safely:** a node that's alone (no peers heard, or all peers' 90 s TTL expired) gets
`live_size = 1` → `fleet_pop_share_k` is the identity function → behaves exactly like standalone.
Losing a peer shrinks the *effective* K within one TTL window (≤90 s), so the remaining nodes
absorb more of the target — correct population-match behavior, not a bug.

## Testing (host, pure — matches the existing `churn_selftest.c` `test_fleet` pattern)

- `fleet_note_peer_node` + `fleet_node_count`: add/refresh does not double-count; TTL expiry drops a
  stale node; table eviction (oldest) when `FLEET_NODE_CAP` is exceeded; independent of the existing
  MAC-exclusion table (adding many synthetic MACs from one node doesn't inflate the node count).
- `fleet_pop_live_size`: returns `1` with no peers noted (boot / standalone); returns `N+1` after
  noting `N` distinct peer MACs; a peer's entry aging past `FLEET_MAC_TTL_MS` drops the live size
  back down.
- `fleet_pop_share_k(target, fleet_pop_live_size(now))` composition: existing `fleet_pop_share_k`
  tests already cover the arithmetic; a small integration test confirms the coexist call site
  compiles and threads the value through (compile-verify, not a new host-executable path since
  `coexist.c` isn't host-compiled).

**HW validation (deferred, not attempted tonight):** proving the live target actually divides
correctly across >1 physical node needs the 3-board fleet up and running, which was deliberately
left un-flashed overnight per the standing "no hardware changes without the user present" rule this
session. The user can validate on return, or keep running static-K if they prefer that stability —
this feature is purely additive to a new call site and does not remove the static path.

## Honest ceilings

- **Wi-Fi-only in v1** — the BLE/persona crowd still needs a re-flash to change fleet size (see
  Scope). A live-resizable persona pool is a clearly-scoped future increment, not implied by this
  work.
- **Convergence lag** — a joining/leaving node takes up to one TTL window (90 s) to be reflected,
  and the *effect* on population only shows at the next Wi-Fi reprofile tick (~5 min) — this is not
  instant, by design (matches the existing reprofile cadence, no new fast-path added).
- **Census counts *radios heard*, not *humans/nodes* in some abstract sense** — a node that's
  powered on but hasn't yet sent its first `FLEET_MACS` broadcast (up to 25 s after boot) isn't
  counted until it does. Acceptable: same order of latency as the existing broadcast cadence.

## Out of scope

- Making the BLE/mesh-persona population live-resizable (needs `phantom_set_target` + a re-sync
  pass — a separate, larger piece of work).
- Any change to the wire format (`RADAR_TYPE_FLEET_MACS` frame content is unchanged; only the
  *sender identity*, already available via `info->src_addr`, is newly consulted).
- CYD-side changes (the CYD's own `node_id_for` mechanism is untouched; this is decoy-side only).
