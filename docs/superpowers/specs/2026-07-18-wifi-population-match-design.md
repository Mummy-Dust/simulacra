# Wi-Fi Population-Match — Design

**Date:** 2026-07-18
**Status:** Approved (design), pending implementation plan
**Roadmap:** Phase 3 (M6 population-match) extended to the Wi-Fi probe layer

## Goal

Drive the probe-agent count from **observed ambient phone density** instead of the fixed
`PROBE_PHONES` (16 on C5 / 8 on C6), so the fake-phone crowd tracks the real crowd. This closes the
primary Wi-Fi tell the probe behavioral audit measures — at home, ~16 fake phones in a near-empty
room gave `density_dominance = 1.0` (over-population). It mirrors the proven BLE M6 population-match
(active-set size driven by the observed `rf_model`).

## Threat model — what this fixes

A passive observer counting probing devices sees the fake-phone population as *too many for the
room*. Matching the observed real-phone density removes the count-based separation. Honest ceiling:
in a truly empty environment the right answer is *few* fake phones — population-match caps
over-population; it cannot manufacture a realistic crowd from nothing.

## Design laws (respected)

1. **Aggregates only (Law 1) — load-bearing here.** Observed real MACs are **hashed-and-dropped**
   (salted, per-boot, exactly like `observe.c` does for BLE): the observe core keeps only a count,
   never a raw bystander MAC.
4. **Population-match (Law 4) — this is the feature.** Active fake phones track observed density and
   never far exceed it.

Laws 2, 3, 5 unaffected (no new emitted identities; observe is receive-only; no raw capture stored).

## Architecture

Mirrors the BLE `observe → model → population-match` pipeline. The estimator is a **pure,
host-testable core**; the promiscuous RX is thin HW glue.

### New module: `main/wifi_observe.{h,c}`

**Pure core (host-testable, no radio):**
- `void wifi_obs_reset(uint32_t salt);` — clear the rolling table, set the per-boot hashing salt.
- `void wifi_obs_note(const uint8_t mac[6], uint32_t now_ms);` — hash the MAC (salted FNV-1a, like
  `observe.c`), add-or-refresh it in a rolling table (cap ~64 entries, TTL `WIFI_OBS_TTL_MS` ~120 s;
  reuse free/expired slots, else evict oldest — same shape as `fleet.c`). The raw MAC is never stored.
- `int wifi_obs_density(uint32_t now_ms);` — count of non-expired distinct hashed MACs = recently
  active real-phone proxy.
- `int wifi_obs_target(uint32_t now_ms);` — EWMA-smoothed density mapped to an agent target:
  `clamp(round(ewma_density), WIFI_OBS_FLOOR, PROBE_AGENTS_MAX)`. The EWMA state advances each call
  (or on an explicit tick — the plan pins which). `WIFI_OBS_FLOOR` default **2** (configurable via
  `-DWIFI_OBS_FLOOR`), a couple of fakes read as plausible almost anywhere and keep some cover.

**HW glue (in this module, gated like the other radio code):**
- A promiscuous `rx_cb` filtering **probe requests** (frame control `0x40`), reading the source MAC
  (offset 10). It calls `wifi_obs_note` for **randomized (locally-administered) source MACs only**
  (the real-phone proxy — bit 0x02 of the first octet), and **skips `fleet_mac_excluded` peers**
  (fleetmate decoys). Our own injected frames are never received (`esp32-no-self-rx`), so a single
  board never counts its own decoys.
- `bool wifi_obs_start(void);` — enable promiscuous on the **existing STA interface** with a mgmt
  filter + the `rx_cb`. Returns `true` iff promiscuous enabled (so `coexist` can pick the fallback
  path on `false`). Continuous; it samples whatever channel injection currently uses (precision is
  not required — order-of-magnitude density is enough).

### Runtime agent target — `main/probe_agents.{h,c}`

Add `void probe_agents_set_target(int n, uint32_t now_ms);` — adjust the live agent set to `n`
(`1..PROBE_AGENTS_MAX`): spawn to grow, retire the excess to shrink. Mirrors
`churn_set_active_target` on the BLE side. `probe_agents_init` keeps setting the initial count.

### Wiring — `main/coexist.c`

- Call `wifi_obs_start()` once, after the STA/injection Wi-Fi side is up; keep its returned status.
- On the existing **re-profile tick**, call `probe_agents_set_target(wifi_obs_target(now), now)` when
  observe started, else `probe_agents_set_target(WIFI_OBS_FALLBACK, now)` — so the fake-phone count
  follows observed density on the same cadence the BLE crowd re-profiles.

## Fallback (safe by construction)

- If `esp_wifi_set_promiscuous` (or the filter/cb setup) **errors**, `wifi_obs_start` logs it and the
  system uses a conservative fixed target `WIFI_OBS_FALLBACK` (default **6**, `< PROBE_PHONES`) — the
  re-profile tick detects "observe unavailable" and calls `probe_agents_set_target(WIFI_OBS_FALLBACK)`.
- If promiscuous enables but **hears nothing**, `wifi_obs_density` reads ~0 → target clamps to the
  floor. That is the correct outcome whether the room is genuinely empty *or* RX is silently broken —
  few agents is safe either way. No separate health-check needed.

## Data flow (per re-profile tick, on the coexist task)

1. Promiscuous `rx_cb` has been folding probe-request source MACs into `wifi_observe` continuously.
2. `t = wifi_obs_target(now)` (EWMA density → clamped target), or `WIFI_OBS_FALLBACK` if observe
   failed to start.
3. `probe_agents_set_target(t, now)` grows/shrinks the live fake-phone set.

## Files

- Create `main/wifi_observe.h`, `main/wifi_observe.c`.
- Modify `main/probe_agents.{h,c}` — add `probe_agents_set_target`.
- Modify `main/coexist.c` — `wifi_obs_start()` + per-tick target update.
- Modify `main/CMakeLists.txt` — add `wifi_observe.c` to SRCS; forward `WIFI_OBS_FLOOR`,
  `WIFI_OBS_FALLBACK`, `WIFI_OBS_TTL_MS` gate flags.
- Host tests for the pure core + `probe_agents_set_target` (via the existing `tools/probe_audit`
  `probe_dump` harness pattern, or a dedicated host test like the other pure cores).

## Testing

**Host (pure, gates the merge):**
- `wifi_observe`: `wifi_obs_note` + `wifi_obs_density` counts distinct MACs; a MAC re-noted refreshes
  (no double count); a MAC past `WIFI_OBS_TTL_MS` expires and drops the count; the same raw MAC is
  never present in the module state (hash-and-drop); `wifi_obs_target` clamps to `[FLOOR, MAX]` and
  tracks the EWMA; table eviction when full.
- `probe_agents_set_target`: growing spawns up to `n`; shrinking retires to `n`; clamps to
  `[1, PROBE_AGENTS_MAX]`; `--agents`-style simulation shows the live count follow a target sequence.

**HW-gated (the real validation, hardware-driven, recorded not merge-blocking):**
- Promiscuous + STA + injection + BLE coexist on one C5/C6 (no crash, injection continues, RX fires).
- Density tracks reality: with N real phones nearby, `wifi_obs_density` ≈ N; agent count follows.
- Re-run the probe behavioral audit on a fresh capture and confirm `density_dominance` drops from 1.0
  toward a plausible value. This is the outcome measure the audit was built for.

## Out of scope (future)

- **Precise device de-duplication** (fingerprint-clustering rotated MACs into devices) — v2; the
  rolling distinct-MAC count is a deliberate rough proxy (rotation over-count is acceptable for
  population-match).
- **Duty-cycled or separate-node observe** (approaches B/C) — A (continuous STA promiscuous) chosen.
- **5 GHz observe** — the 2.4 GHz probe population is the proxy; band coverage follows injection's
  current channel. Not separately scanned.
- **Directed-probe SSID realism** — a separate, audit-gated piece (constellation-safe per-persona PNL;
  design insight banked in `private/KISMET-VALIDATION.md`).
