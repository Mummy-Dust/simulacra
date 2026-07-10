# Wi-Fi Probe Behavioral Realism — Design

**Date:** 2026-07-10
**Status:** Approved (design) — implementation plan to follow
**Builds on:** `2026-07-10-probe-archetype-realism-design.md` (the per-frame *content* layer)
**Milestone scope:** Wi-Fi probe-request behavior only. BLE PAN camouflage is a separate follow-on design.

## 1. Problem

The archetype work made each injected probe request *look* like a real phone at the byte level
(HT/VHT/HE + vendor IEs, four weighted archetypes, validated byte-exact on-air). But the injector's
**behavior** is trivially synthetic, and against a fingerprinting adversary the behavior is the
weaker half. Today `probe_inject_burst(channel)` loops **every** fake phone onto **one** channel in a
single tight burst, on a fixed period, from a **fixed pool** of N MACs that never turns over.

This produces four independently fatal tells:

1. **Sequence-number linking.** The 802.11 sequence counter links frames to a physical radio. The
   current code transmits with the chip's internal counter enabled, so all fake phones on a board
   share **one monotonic counter**. (Confirmed empirically: in a single captured instant, seven
   MACs spanning all four archetypes emitted a perfectly **consecutive** sequence run; each board
   showed its own consecutive run; a genuine ambient device sat on a separate, unrelated counter.)
   A one-line clustering rule collapses every fake phone on a board into a single device — and the
   perfect consecutiveness *also* flags them as synthetic, since no real crowd counts in lockstep.
2. **Timing correlation.** All MACs fire in the same millisecond on a metronomic period. Real phones
   are on independent clocks; their probe arrivals are uncorrelated and irregular.
3. **Fingerprint re-linking across MAC changes.** A fixed pool with stable per-MAC fingerprints lets
   an adversary re-link a "new" randomized MAC to an old one by matching the fingerprint + cadence.
4. **Constellation / co-occurrence tracking.** The adversary fingerprints the *set of devices that
   recur around the target*. A fixed set of decoys that always accompanies the operator's phone
   becomes a **portable license plate** — strictly worse than no decoys.

## 2. Goal

Turn the fixed, lockstep pool into a **population of independent, short-lived agents** so that a
professional passive surveillance tool (SignalTrace class) cannot:
- link fake MACs by sequence number,
- cluster them by synchronized/periodic timing,
- re-link them across MAC changes by stable fingerprint,
- or fingerprint the operator by the recurring set of decoys around them.

Device-side hardening (randomized MACs, radios off when idle) is assumed already in place; this is
the environmental layer on top of it.

## 3. Honest limitation (named, not a footnote)

This design defeats layers #1–#4 above. It **does not** defeat the physical layer: **RSSI, angle of
arrival, and co-location.** Every agent on one board shares a single fixed position and near-constant
signal strength; a multi-sensor or AoA-capable receiver still separates "signals emanating from a few
fixed points" from "the device that moves with the target." Per-agent TX-power jitter blurs the
signal-strength tell but cannot manufacture spatial separation. Furthermore, the operator's real
device still *does real things* decoys cannot fake (it associates, passes traffic, moves, and
persists across time and place).

**Decoys enlarge the haystack and raise the adversary's cost. They are not invisibility.** This
limitation must remain stated in every artifact derived from this spec.

## 4. Architecture

Follow the pattern proven by the archetype work: a **pure, host-testable core** plus a **thin live
glue** layer, so every scheduling decision is unit-tested off-target with a seeded RNG.

- **`main/probe_agents.{c,h}` (new, pure).** Owns the agent array, the per-agent sequence counters,
  the scan schedule, the duty-class model, and the birth/death lifecycle. Contains **no ESP-IDF
  calls** — RNG and clock are injected — so it compiles and runs under `tools/probe_audit` on the
  host. It answers two questions each tick: *which agents emit now* and *what identity does each
  emit with*.
- **`main/probe.c` (live glue).** Keeps Wi-Fi bring-up and the raw `esp_wifi_80211_tx` path.
  `probe_inject_burst` is replaced by a tick entry point that asks the pure core for the due agents
  on the current channel and transmits their frames with the chip's internal sequence counter
  **disabled** (`en_sys_seq = false`), stamping each agent's own sequence number.
- **`main/coexist.c` (integration).** Unchanged ownership of radio/BLE arbitration. The existing
  `fire_wifi` branch calls the new tick instead of the all-phones burst. The C5 5 GHz excursion path
  routes through the same tick.

### Radio-arbitration constraint

`coexist.c` owns the radio and must protect BLE (on the C5, tuning to 5 GHz silences BLE, and
flooding the Wi-Fi TX buffer returns `ESP_ERR_NO_MEM`). The agent core therefore **proposes**
emissions; the coexist scheduler still decides **when** a Wi-Fi excursion happens and **on which
band**. The agent core must be correct for "you may emit up to K frames on channel C now," not
"seize the radio." True per-scan channel sweeps are constrained by BLE radio-sharing and are treated
as best-effort, not a guarantee.

## 5. The agent model

Each agent is a small state machine:

```
typedef enum { DUTY_ACTIVE, DUTY_IDLE } duty_class_t;

typedef struct {
    uint8_t      mac[6];        // locally-administered, held for the agent's whole life
    probe_arch_t arch;          // bound to the MAC for its life (real phones don't reshuffle model)
    uint16_t     seq;           // 12-bit; random base, +1 per frame THIS agent sends
    duty_class_t duty;          // few ACTIVE (frequent), most IDLE (rare)
    uint32_t     next_scan_ms;  // jittered; when this agent is next due to probe
    uint32_t     born_ms;
    uint32_t     life_ms;       // bounded lifetime; on expiry the agent dies and is replaced
    bool         alive;
} probe_agent_t;
```

**Per tick (pure core, given `now_ms` and an "emit budget" for a channel):**
1. **Lifecycle.** Any agent past `born_ms + life_ms` **dies** (goes silent forever — its MAC never
   returns). Dead slots are **reborn** with a fresh MAC, freshly-drawn archetype, fresh random seq
   base, duty class, and lifetime — so the population continuously turns over and the constellation
   never repeats. Live count is allowed to fluctuate within a band, not pinned to a constant.
2. **Due selection.** Return the subset of live agents whose `next_scan_ms <= now`. This is a
   **partial, uncorrelated subset** — never "all agents." If none are due, emit nothing this tick.
3. **Reschedule.** Each emitting agent gets a new `next_scan_ms = now + base(duty) + jitter`, where
   ACTIVE agents scan on a short interval and IDLE agents on a long one, both with wide jitter.
4. **Identity.** For each emitting agent the glue builds its archetype frame with its own MAC and
   stamps its own `seq`, then `seq = (seq + 1) & 0x0FFF`.

**Churn philosophy (locked):** an agent holds one MAC for its entire life — matching real phones,
which do **not** reshuffle their randomized MAC every few seconds. The constellation churns through
**population turnover** (births/deaths on minute-scale lifetimes), *not* through fast per-MAC
rotation. This simultaneously satisfies "each phone individually realistic" and "the set never
stabilizes."

## 6. Sequence-number realism

Owning the counter must reproduce *real* behavior, not merely avoid consecutiveness:
- **Within an agent:** monotonic `+1` per transmitted frame, wrapping at 12 bits. (Random per-frame
  sequence numbers would be their own fingerprint — real radios are monotonic.)
- **Across agents:** independent counters with random starting bases, so no cross-MAC run appears.

This reproduces exactly what a genuine independent device looks like (its own isolated, monotonic
counter), which is what the adversary expects to see for distinct physical radios.

**Hardware-validation gate (highest risk).** Some ESP32 builds override the sequence field even when
`en_sys_seq = false`. This must be **validated on the C5 and C6 before anything else is built**: a
minimal test that injects frames with distinct, known sequence numbers, captured and confirmed
on-air. If the chip ignores the flag, the sequence-independence pillar is not achievable in firmware
and the milestone is re-scoped before further work. This is the plan's first gated task.

## 7. Validation bar

Re-capture with Kismet after implementation and measure off the pcapng. The `scratchpad` analysis
scripts graduate into permanent analyzers under `tools/probe_audit`:
- **Sequence independence** — no consecutive/interleaved sequence runs across distinct decoy MACs
  (the exact condition that currently fails).
- **Timing decorrelation** — inter-arrival distribution shows no dominant fixed-period spike and no
  volleys where many MACs fire in the same instant.
- **Constellation churn** — the set of live decoy MACs turns over across the capture window; no
  stable recurring set persists end-to-end.

## 8. Testing strategy

The pure `probe_agents.c` core is deterministic under a seeded RNG, so the host harness
(`tools/probe_audit`) covers, without hardware:
- **Due selection never returns the whole population** in a single tick, across many seeds.
- **Sequence numbers** are monotonic within an agent and independent (no shared run) across agents.
- **Lifecycle turnover** — over simulated time the live-MAC set reshapes; no MAC persists beyond its
  lifetime; dead MACs never reappear.
- **Duty mix** — the ACTIVE/IDLE split holds and produces the intended cadence separation.
- **Emit budget respected** — the core never asks the glue to exceed the per-excursion frame budget.

On-target, a short serial-log smoke check confirms the tick logs due-subset sizes and per-agent
births/deaths, and the seq-injection gate (§6) confirms on-air sequence control.

## 9. Non-goals (YAGNI)

- **No BLE changes.** The BLE PAN-camouflage layer is a separate milestone.
- **No physical-layer claims.** RSSI/AoA/co-location are out of scope and unfixable with this
  hardware (§3). TX-power jitter is optional hardening, not a solution.
- **No capture-driven parameters yet.** Driving timing/duty/mix from real-capture statistics is a
  documented phase-2 hook, not built here.
- **No perfect per-scan channel sweeps.** Constrained by BLE radio-sharing; best-effort only.
- **No new inter-board coordination protocol.** Boards run the agent core **autonomously** (works
  with no CYD in range) and are deliberately **un-synchronized** with each other.

## 10. Global constraints

- **Public repo.** No absolute local paths, OS usernames, real hardware MACs, or real SSIDs in any
  committed file. Real-capture artifacts stay in the gitignored `private/`, PII-stripped.
- **Targets.** ESP32-C5 (IDF 5.5, 8 agents, 2.4 + 5 GHz) and ESP32-C6 (IDF 5.4, 4 agents, 2.4 GHz).
  The agent core is target-agnostic; per-target counts stay compile-time as today.
- **Law 3 preserved.** Probe requests remain wildcard-SSID only; the archetype content layer and its
  byte-exact fixtures are unchanged by this milestone.
- **Host-testable core.** New scheduling logic lives in a pure module with injected RNG/clock and is
  unit-tested under `tools/probe_audit`; live radio code stays out of the pure core.

## 11. Open risks

- **`en_sys_seq=false` support** (§6) — the gating unknown; validated first.
- **Emit budget vs. realism** — too small a per-excursion budget starves the crowd; too large
  re-creates a volley. Tuned against the coexist tick/BLE budget during implementation.
- **Lifetime tuning** — lifetimes too short make MACs vanish unrealistically fast; too long lets the
  constellation stabilize. Start with minute-scale, refine against a capture.
