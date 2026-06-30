# M8 — BLE + Wi-Fi Coexistence, Live Re-Profiling & Anti-Entourage (Design)

**Status:** Accepted (design) — 2026-07-01
**Milestone:** M8 (Simulacra v2 synthetic-population engine)
**Predecessors:** M3 churn engine, M4 payload templates, M5 observe→model, M6 generate+population-match, M7 Wi-Fi probe injection
**Personas:** Ward (ESP32-C5, fixed/vehicle) and Shade (ESP32-C6, portable EDC) — see the persona split.

## Context & goal

Through M7 the BLE decoy and the Wi-Fi probe injector each work, but in **mutually
exclusive build modes**: `app_main` early-returns for `SIMULACRA_PROBE`, so NimBLE is
never started when Wi-Fi runs, and vice-versa. M8 makes them run **concurrently on one
ESP32-C5/C6** so the device emits a combined synthetic crowd (fake BLE devices + fake
phone Wi-Fi traffic) at the same time, and makes that crowd **adapt** to the room in real
time without a reflash.

M8 has three layers, built and accepted in order so the device stays flashable and working
after each:

1. **Coexistence + duty-cycle** — both radios live at once via ESP-IDF software
   coexistence, with a coordinator that owns the coarse per-persona airtime budget.
2. **Live re-profiling** — the device periodically re-observes the room (reusing the M5
   observe machinery while still advertising) and reshapes its synthetic population
   (reusing M6 generate/resize), gated by a periodic floor **and** a drift trigger.
3. **Shade anti-entourage** — when drift detection says the portable Shade moved to a new
   environment, accelerate churn so the carried-over population rotates out gradually and a
   fresh room-matched population phases in, so the fake crowd never becomes a signature that
   follows you.

## Global constraints (carried from prior milestones — apply to every task)

- **Refined Law 3 (BLE):** iBeacon (Apple `0x004C`, subtype `0x02`, len `0x15`) IS allowed
  (silent location beacon). NEVER emit Apple Continuity proximity-pairing `0x07`,
  nearby-action `0x0F`, Find My `0x12`, Microsoft `0x0006`, or Google `0xFE2C` (popup
  formats). Advertising is **non-connectable** only.
- **Wi-Fi Law 3:** wildcard-SSID (broadcast) probe requests ONLY. Never directed / named-SSID
  / SSID-list probes.
- **Data discipline:** the `rf_model` stays aggregates-only (no addr/name/payload fields).
  The observe dedup table is RAM-only, per-boot salted, and wiped each sweep. No raw capture
  in shipping builds.
- **Personas:** behavior differentiates by `#if CONFIG_IDF_TARGET_ESP32C5` (Ward) vs C6
  (Shade). Ward = dense, mains-powered, dual-band (2.4 + 5 GHz). Shade = lean,
  battery-conscious, 2.4 GHz only.
- **Toolchain:** Ward/C5 builds under ESP-IDF **v5.5** (`~\esp\v5.5\esp-idf`,
  target esp32c5, COM12), Python 3.12 forced on PATH. Shade/C6 builds under ESP-IDF **v5.4**
  (target esp32c6). XIAO C6 needs `-DSIMULACRA_BOARD_XIAO_C6=1`.
- **Project rule:** keep files under 500 lines; typed interfaces; TDD (host self-test first).

## Architecture (Approach A: coordinator + ESP-IDF SW coexistence)

Enable the platform's software coexistence layer — both the Wi-Fi and the BT controllers are
initialized, and `esp_coex` arbitrates microsecond-level radio access automatically. We add
**one** coordinator module (`coexist.c`) that owns the *coarse* duty-cycle: it drives the BLE
churn engine continuously, gates Wi-Fi probe bursts and (Ward) 5 GHz excursions into
scheduled windows to hit the persona airtime budget, and runs the periodic re-profile + drift
slot. The platform handles the hard real-time arbitration; our code only does coarse
scheduling and reuses the M3–M7 modules wholesale.

**Rejected alternatives:** strict app-level TDM (constant stop/restart of both stacks fights
esp_coex, and ext-adv stop/restart is exactly what threw HCI errors earlier); two
free-running tasks with no coordinator (no airtime-budget control, nowhere clean to host
re-profiling/drift/anti-entourage).

**Risk to retire first (Layer 1 acceptance gate):** our Wi-Fi use is injection-only (no AP
association). Coexistence operates at the controller level so it should be unaffected, but the
combined "both radios live, no watchdog/crash over a sustained run" proof is the gate before
Layers 2–3 are built on top.

## Module structure & build modes

**New modules**
- `coexist.{h,c}` — the coordinator: scheduler task, duty-cycle state machine, per-persona
  airtime budget. Calls churn (BLE), probe (Wi-Fi), observe/generate (re-profile), drift
  (anti-entourage). Public: `coexist_start(void)`.
- `drift.{h,c}` — drift metric. `float drift_score(const rf_model_t *prev, const rf_model_t
  *cur)` = vendor-mix L1 distance + normalized distinct-count delta; `bool
  drift_exceeds(float score, float threshold)`. Pure function; no radio. Serves both
  re-profiling and anti-entourage.

**Modified**
- `probe.{h,c}` — extract `void probe_inject_burst(uint8_t channel)` (one burst of N fake
  phones on a chosen channel) + MAC-pool management as a scheduler-callable core. Keep
  `probe_start()`'s forever-loop as a thin wrapper over the core for the `SIMULACRA_PROBE`
  dev mode.
- `observe.{h,c}` — add `void observe_window(uint32_t duration_ms)`: scan via
  `ble_gap_ext_disc` for a bounded window **while advertising continues**, then
  `observe_end_sweep`. Distinct from the standalone forever-observe mode.
- `churn.{h,c}` — add a runtime retirement-rate boost: `void churn_set_accel(float mult)`
  (multiplier ≥ 1.0 applied to dwell/cooldown shortening) that the coordinator decays back to
  1.0. Default 1.0 = today's behavior.
- `simulacra_main.c` — the **default** path (all build flags 0) becomes: NVS → start NimBLE
  (broadcaster + observer) → init Wi-Fi STA under coexist → `coexist_start()`. The existing
  `SIMULACRA_PROBE` / `SIMULACRA_SNIFF` / `SIMULACRA_OBSERVE` early-returns and
  `CHURN_SELFTEST` stay as single-stack dev/verification builds.
- `sdkconfig.defaults.esp32c6` / `.esp32c5` — enable SW coexistence (exact Kconfig symbol,
  e.g. `CONFIG_ESP_COEX_SW_COEXIST_ENABLE`, confirmed against the installed IDF at plan time;
  Wi-Fi must be enabled alongside BT).
- `main/CMakeLists.txt` — add `coexist.c`, `drift.c` to SRCS (REQUIRES already covers
  `bt nvs_flash driver esp_wifi esp_netif esp_event`).

**Build modes after M8**
| Flag (all default 0) | Behavior |
|---|---|
| *(none set)* | **Combined coexist decoy — BLE + Wi-Fi together (new default)** |
| `SIMULACRA_PROBE` | Wi-Fi-only probe injector (dev) |
| `SIMULACRA_SNIFF` | Wi-Fi probe sniffer (verification / M9 seed) |
| `SIMULACRA_OBSERVE` | BLE-only ambient observe (dev) |
| `CHURN_SELFTEST` | On-target host-logic self-test (radio idle) |

## Layer 1 — Duty-cycle scheduler (`coexist.c`)

One coordinator FreeRTOS task, tick loop, `vTaskDelay` every tick (watchdog-safe). Each tick:
- **Always** drive `churn_tick(now)` — BLE ext-adv is gap-friendly and runs continuously
  under coexist.
- On the **Wi-Fi cadence**, fire one `probe_inject_burst(ch)` (channel from the persona's
  hop set; Ward interleaves a 5 GHz excursion per Layer-1 5 GHz handling below).
- On the **re-profile cadence**, run Layer 2.

Persona config struct selected by `#if CONFIG_IDF_TARGET_ESP32C5`:

| Knob | Ward (C5) | Shade (C6) |
|------|-----------|------------|
| Wi-Fi burst period | ~2 s (heavier) | ~6–8 s (thin) |
| 5 GHz excursions | yes | no |
| Re-profile period | ~10 min | ~5 min |
| Drift accel threshold | high (≈ off, stationary) | active (anti-entourage) |

Exact constants are locked in the implementation plan; the table fixes the relative shape
(Ward dense + dual-band, Shade lean + 2.4-only).

### Layer 1 — C5 5 GHz handling

The one exclusion `esp_coex` cannot arbitrate: when the C5 tunes to a 5 GHz channel to
inject, the radio is physically on 5 GHz and BLE (2.4 GHz) cannot TX during that window. The
scheduler therefore batches 5 GHz into **short bounded windows** — one quick burst across the
5 GHz channel set, then immediately retune to 2.4 GHz so BLE adv resumes — and keeps them
sparse relative to 2.4 GHz to protect BLE airtime. The 2.4 GHz BLE-vs-Wi-Fi interleave is
left to the coex arbiter; the band excursion is the scheduler's explicit responsibility
because it is a hard tuning exclusion, not arbitration.

## Layer 2 — Live re-profiling

On the re-profile cadence the coordinator:
1. Runs `observe_window(OBS_SWEEP_MS)` (~15 s) — scans via `ble_gap_ext_disc` **while
   advertising continues** (both NimBLE roles compiled in). `observe_ingest` dedups + bins as
   in M5.
2. `observe_end_sweep` → updates the EWMA `rf_model` (RAM; persisted to NVS opportunistically
   as M5 does).
3. Snapshots the pre-update model, computes `drift_score(prev, cur)`.
4. **Reshapes:** `generate_roster` from the fresh model + recompute persona active-target
   (`generate_active_target` → `churn_set_active_target`). The reshape flows through churn
   **gradually** — fresh identities phase in as churn promotes them, old ones age out on
   normal dwell/cooldown. No hard swaps.

If `total_obs < GEN_MIN_OBS` (too few sightings this cycle), skip the reshape and keep the
current population.

**To validate early on hardware:** concurrent `ble_gap_ext_disc` + `ble_gap_ext_adv` (scan +
advertise simultaneously). Both roles are enabled; expected clean.

## Layer 3 — Shade anti-entourage (accelerated gradual rotation)

Active on **Shade (C6) only**. After each re-profile, if `drift_exceeds(score,
SHADE_DRIFT_THRESHOLD)` (environment changed → you moved):
- `churn_set_accel(~3.0)` — dwell windows shrink and fresh-identity promotion speeds up, so
  the carried-over population ages out fast and the already-reshaped room-matched population
  (Layer 2) phases in over ~1–2 min, **not** instantly.
- The coordinator **decays the boost linearly back to 1.0** over a fixed transition window
  (`SHADE_ACCEL_DECAY_MS`, on the order of ~2 min), so normal churn resumes once you've
  settled into the new room.
- Ward uses an effectively-off threshold — it is stationary, so the periodic refresh suffices.

Net: no synthetic MAC survives a location change for long, **and** the transition reads as
natural crowd turnover (gradual), avoiding the "N fake devices teleport in on arrival" tell.

## Error handling

- **Coexist / Wi-Fi init failure** → log and **fall back to BLE-only decoy** (never brick the
  mature core); the device still advertises.
- **`probe_inject_burst` tx_rc != 0** → log and continue (transient).
- **Re-profile with `total_obs < GEN_MIN_OBS`** → skip reshape, keep current population.
- **Coordinator task** → `vTaskDelay` every tick; same watchdog-safe pattern as the existing
  churn loop.

## Testing & acceptance

**Host self-tests** (`CHURN_SELFTEST=1`, no radio) — added alongside the existing suite, all
`PASS (fails=0)`:
- `test_drift` — identical models → score 0; disjoint vendor mixes → high score; ordering
  monotonic with mix distance.
- `test_scheduler_budget` — persona tick math selects Wi-Fi and re-profile slots at the right
  cadence for Ward vs Shade.
- `test_accel_rotation` — `churn_set_accel(3.0)` shortens effective dwell; boost decays back
  to 1.0.

**Hardware acceptance (headline proofs):**
1. **Coexistence (both at once)** — flash default build: nRF Connect sees the random-static
   (≥0xC0) decoys advertising **AND** the XIAO C6 sniffer (`SIMULACRA_SNIFF`) sees randomized
   probe-reqs **simultaneously**, sustained run, no watchdog/crash. *This is THE M8 proof.*
2. **Live re-profiling** — with the device running, change the room (or advertise different
   vendors nearby) → serial shows the model update + population reshape, no reflash.
3. **Anti-entourage (Shade)** — trigger an environment change → serial shows drift over
   threshold, accelerated rotation (old MACs retiring fast, fresh ones phasing in), boost
   decaying back to 1.0.

Default build ships the combined coexist decoy.

## Out of scope (deferred)

- **Wi-Fi observe→model→match** — the natural **M9**: model real probe traffic (the M7
  `sniff.{h,c}` tool is the seed) and shape injected probes to match, as M5/M6 did for BLE.
  Until then the model stays BLE-derived and Wi-Fi probing stays template/heuristic.
- **Deep 5 GHz regulatory / rate tuning** beyond basic channel hopping.
- **IMU / movement sensing** — drift detection already gives RF-environment movement-awareness
  with no extra hardware.
