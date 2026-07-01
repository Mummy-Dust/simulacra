# M9 — Passive Tracker / Follower Detection ("Threat Radar") (Design)

**Status:** Accepted (design) — 2026-07-01
**Milestone:** M9 (Simulacra v2 synthetic-population engine)
**Predecessors:** M3 churn engine, M4 payload templates, M5 observe→model, M6 generate+population-match, M7 Wi-Fi probe injection, M8 coexistence + live re-profiling + anti-entourage
**Personas:** Ward (ESP32-C5, fixed/vehicle) and Shade (ESP32-C6, portable EDC) — see the persona split.

## Context & goal

Through M8 the device is a combined BLE + Wi-Fi decoy that continuously re-profiles the
room and reshapes its synthetic crowd. Everything so far is **emissive** — it makes the
device harder to single out. M9 turns the same sensing **inward-out**: while it decoys, the
device also **watches for a device that is following *you*** — a stable-identity beacon seen
with you across multiple distinct environments — and raises a **threat alert** so you can
locate it.

M9 is **behavioral** ("this device moves with me") rather than fingerprint-based. It keys on
a **stable device identity**, so it catches **non-rotating** followers (a cheap fixed-MAC
beacon slipped into a bag, a stalker's always-on tag that doesn't rotate). MAC-rotating
commercial tags (AirTag / SmartTag / Tile) rotate their addresses and are **not** caught by
this layer — known-tracker fingerprinting is the separately-scoped **M10** detection layer.
This scope boundary is stated plainly everywhere; over-claiming is the failure mode.

The whole capability is **Approach A (piggyback):** it taps data the M8 pipeline already
collects and reuses the M8 re-profile/drift machinery, so it adds **zero** new radio or
coexistence load.

## Global constraints (carried from prior milestones — apply to every task)

- **Data discipline (main pipeline untouched):** the `rf_model` stays aggregates-only; the
  observe dedup table is RAM-only, per-boot salted, wiped each sweep. M9 introduces **one
  deliberate, documented departure** for the detector only (see §2): a per-install salt and,
  for **confirmed threats only**, retention of a live raw MAC so the user can locate the
  device. Candidates remain hashed.
- **Personas:** behavior differentiates by `#if CONFIG_IDF_TARGET_ESP32C5` (Ward) vs C6
  (Shade). Detection thresholds are **persona-shared** (the balanced decision); only the
  re-profile cadence differs, which falls out of M8.
- **Toolchain:** Ward/C5 builds under ESP-IDF **v5.5** (target esp32c5, COM12), Python 3.12
  forced on PATH. Shade/C6 builds under ESP-IDF **v5.4** (target esp32c6). XIAO C6 needs
  `-DSIMULACRA_BOARD_XIAO_C6=1`.
- **Project rule:** keep files under 500 lines; typed interfaces; TDD (host self-test first).
- **Non-derivative:** concepts reimplemented from scratch; no code copied from any sibling
  fork.

## Architecture (Approach A: piggyback observe + reuse M8 drift for epochs)

The detector is a **pure host-logic core** (`detect.c`) fed by two events the M8 pipeline
already produces, with **no radio of its own**:

1. **Per-report tap.** `observe_gap_event` (`observe.c`) already holds the raw MAC
   (`d->addr.val`), RSSI (`d->rssi`), and parsed company **before** `observe_ingest` hashes
   it away. `observe.c` gains a registered **report callback** `(mac, rssi, company)` that
   fires there; the detector subscribes. Observe stays decoupled — it knows nothing about
   detection.
2. **Location-epoch tap.** `coexist_reprofile` (`coexist.c`) already computes
   `score = drift_score(&prev, cur)`. Detection reuses that exact score: when it exceeds a
   detection-owned threshold, the coordinator bumps a location-epoch counter and notifies the
   detector.

A **location-epoch** is "an RF-drift-separated stretch of time" — an RF-neighbourhood proxy,
not GPS. A device that is with you across **3 distinct epochs with meaningful presence** is a
confirmed follower.

**Rejected alternatives:** a dedicated detection scan (adds radio + coex load M8 was built to
avoid); IMU/GPS "did I move" sensing (extra hardware — drift already gives RF
movement-awareness for free); fingerprint-first detection (that is a different, harder axis —
deferred to M10, and it does not catch fixed-MAC followers this layer does).

## Module structure & build modes

**New module**
- `detect.{h,c}` — the pure testable core. Public surface:
  - `detect_observe(uint32_t hash, int8_t rssi, uint16_t vendor, uint16_t epoch)` → returns a
    result enum (`DETECT_NONE` / `DETECT_CONFIRM` = crossed the threshold on this call /
    `DETECT_KNOWN` = a subsequent sighting of an already-confirmed threat) so the caller can
    log and drive locate updates without the core touching NVS/GPIO/raw-MAC.
  - `detect_on_epoch_change(uint16_t epoch)` — bookkeeping (per-epoch counters + candidate
    aging) when the coordinator advances the epoch.
  - `detect_set_enabled(bool)` / compile default `SIMULACRA_DETECT` (default 1).
  - `detect_drain_pending(void)` → returns whether a newly-confirmed threat is pending; drained
    by `coexist_task` to do the deferred NVS persist + LED update outside the BLE callback
    (see §4).
  - Threshold logic lives entirely here, separated from radio and NVS.

**Wiring adapter (in `simulacra_main.c` / `coexist.c`, not in the pure core).** A thin glue
layer owns everything that must stay out of `detect.c` so the core remains pure and
host-testable: at init it loads the **per-install salt** (§2) from NVS; on each observe report
`(mac, rssi, company)` it (1) drops the report if `mac` is in the churn/roster self-exclusion
set, (2) hashes `mac` with the salt, (3) reads the coordinator's current epoch, then calls
`detect_observe(hash, rssi, company, epoch)`. On a `DETECT_CONFIRM` / `DETECT_KNOWN` return it
retains the **live raw MAC** in the RAM threat record (RAM-only, confirmed threats only) and
drives the throttled `THREAT locate` output. Because the pure core only ever sees a hash, no
raw identifier enters its decision logic or the NVS-persisted record.

**Modified**
- `observe.{h,c}` — add a registered report callback fired from `observe_gap_event` with
  `(mac, rssi, company)` *before* the MAC is hashed away. Observe stays decoupled (no
  dependency on `detect`).
- `coexist.{h,c}` — owns the `uint16_t` epoch counter; in `coexist_reprofile`, after
  `drift_score`, bump the epoch on a detection-drift threshold and call
  `detect_on_epoch_change`; drain `detect_drain_pending()` each `coexist_task` tick. Self-MAC
  exclusion list (churn/roster active set) is consulted before forwarding a report.
- `simulacra_main.c` — the **default** combined-decoy path initializes the detector and wires
  observe's report callback to it.
- `main/CMakeLists.txt` — add `detect.c` to SRCS (REQUIRES already covers `nvs_flash`).

**Build modes after M9** (unchanged set; detection is on inside the default decoy)
| Flag (all default 0 except `SIMULACRA_DETECT`) | Behavior |
|---|---|
| *(none set)* | **Combined coexist decoy + Threat Radar (new default)** |
| `SIMULACRA_DETECT=0` | Combined decoy with detection compiled out (zero overhead) |
| `SIMULACRA_PROBE` / `SIMULACRA_SNIFF` / `SIMULACRA_OBSERVE` | single-stack dev modes (unchanged) |
| `CHURN_SELFTEST` | On-target host-logic self-test (radio idle) |

## §1 recap — module boundaries

`detect.c` is the pure core (no radio, no NVS calls inside its decision logic). `observe.c`
publishes reports through a callback and knows nothing about detection. `coexist.c` owns the
epoch counter and the deferred-work drain. Alerting is serial `THREAT:` lines plus an
optional board-gated LED. Detection is default-on and runtime-toggleable via API.

## §2 — Store & data discipline

- **Per-install salt** (persisted once to NVS), **distinct from observe's per-boot salt**, so
  a device's hash is **stable across sweeps and reboots** — required for "same device across
  epochs" to mean anything. This is the one deliberate opt-in departure from the
  aggregates-only pipeline; the main `rf_model` path is untouched.
- **Candidates live in RAM**, hashed: `{ hash, cur_epoch, sightings_this_epoch, credited,
  distinct_epochs, total, first_ms, last_ms, best_rssi }`, LRU-evicted when the table is full.
- **Confirmed threats persist to NVS** (hashed id + credited epochs + first/last + RSSI) — and
  **only** at confirmation does the detector begin retaining the device's **live raw MAC** on
  subsequent sightings, so the user can physically locate it. Raw MAC is never retained for
  unconfirmed candidates.
- **Self-exclusion:** the coordinator skips our own live decoy MACs (from the churn/roster
  active set) before a report reaches `detect_observe`, so the decoy never flags itself.
- **Allowlist (v1-minimal):** alert-once behavior plus an optional static config list. Rich
  "mark trusted" management is deferred to the web-UI milestone.

## §3 — Location-epochs & the follower threshold

**Location-epoch** = a `uint16_t` counter owned by `coexist.c`, starting at 0, incremented
**only** when a re-profile measures a materially changed environment. In `coexist_reprofile`,
immediately after `score = drift_score(&prev, cur)`:

```
if (prev.sweeps > 0 && score > DETECT_EPOCH_DRIFT) {
    s_epoch++;
    detect_on_epoch_change(s_epoch);
}
```

- `DETECT_EPOCH_DRIFT = 0.45` is a **detection-owned** constant, deliberately separate from
  the anti-entourage `drift_thresh` (Shade 0.45 / Ward 2.0 = off). Epochs must advance on
  **both** personas, so this cannot reuse Ward's disabled anti-entourage threshold.
- `prev.sweeps > 0` reuses the existing day-one guard so the first-ever model populate is not
  mistaken for a location change.
- Epochs are **inherently rate-limited to ≤1 per re-profile** (≤1 per 5 min Shade / 10 min
  Ward) — no extra debounce needed.
- **Timing:** the bump happens *after* `observe_window` returns and drift is computed, so
  reports gathered during a window carry the epoch established at the **end of the previous**
  re-profile — i.e. "the location-stretch we are in now." The transition to the next epoch is
  applied once the window closes and drift confirms the environment changed.

**Meaningful presence, per epoch.** Each candidate tracks `sightings_this_epoch`. It earns
**one presence-credit** for an epoch the moment it reaches `DETECT_MIN_SIGHTINGS = 2` adv
reports within that epoch — a noise floor (a ~15 s window at 100 ms–1 s advertising intervals
yields ~15 reports, so a genuinely-present device clears it trivially; a single edge-of-range
blip does not).

**The strike rule.** `distinct_epochs` counts epochs where the device earned presence-credit;
at `DETECT_EPOCH_STRIKES = 3` → **confirmed follower**. Crediting is **immediate** (fires
inside `detect_observe` when the 2nd sighting of a new epoch arrives), so confirmation
triggers *while you are still in* the 3rd distinct location — not one location later.

**Core logic (pure):**
- `detect_observe(hash, rssi, vendor, epoch)`: find/create candidate (LRU); if
  `epoch != cur_epoch` → roll (`cur_epoch = epoch`, `sightings_this_epoch = 0`,
  `credited = false`); `sightings_this_epoch++`; update `total`/`first_last`/`best_rssi`; if
  `!credited && sightings_this_epoch >= DETECT_MIN_SIGHTINGS` → `credited = true`,
  `distinct_epochs++`, and if `distinct_epochs >= DETECT_EPOCH_STRIKES` return
  **`DETECT_CONFIRM`**.
- `detect_on_epoch_change(epoch)`: bookkeeping only — bounds RAM by aging out un-promoted
  candidates unseen for `AGE_OUT_EPOCHS = 8` (a follower recurs; a left-behind device does
  not).

**Why this catches both threat shapes.** *Stationary-with-you* (beacon in your bag): it moves
into each new epoch as you move, re-crediting each new place. *Mobile follower* (walks/drives
with you): each of your moves spikes drift → new epoch → the follower clears the 2-report
floor in every ~15 s window. A **drive-by** (in range for one window at one place) earns at
most one epoch-credit → never confirms.

| Constant | Default | Meaning |
|---|---|---|
| `DETECT_EPOCH_DRIFT` | 0.45 | drift score above which a re-profile = new location-epoch (both personas) |
| `DETECT_MIN_SIGHTINGS` | 2 | adv reports in an epoch to earn presence-credit (noise floor) |
| `DETECT_EPOCH_STRIKES` | 3 | distinct credited epochs → confirmed follower |
| `AGE_OUT_EPOCHS` | 8 | drop an un-promoted candidate unseen this many epochs (RAM bound) |

Thresholds are **persona-shared** (the balanced decision); Ward's slower re-profile cadence
means it accumulates epochs over more ground per unit time, which suits a vehicle — no
separate knob (YAGNI).

## §4 — Alerting

**Serial `THREAT:` lines (primary, always available)** — parseable/greppable by the existing
pyserial readers. Two event types:

- **`THREAT confirmed`** — emitted **once** when a candidate crosses `DETECT_EPOCH_STRIKES`:
  ```
  THREAT confirmed id=3f9c vendor=0x0075 epochs=3 first=... last=... rssi=-58 mac=CC:0A:..
  ```
  `id` = short hash prefix (privacy-preserving handle); `mac` = the live raw MAC, captured
  only now that it is a confirmed threat (§2), so the user can physically locate it.
- **`THREAT locate`** — ongoing, **RSSI-throttled** updates on subsequent sightings of an
  already-confirmed threat, for "getting-warmer" physical search:
  ```
  THREAT locate id=3f9c rssi=-42 seen=+12s
  ```
  Rate-limited: emitted only when RSSI moves ≥ `DETECT_LOCATE_RSSI_DELTA` (6 dB) **or** every
  `DETECT_LOCATE_MIN_MS` (10 s), whichever first — no per-packet spam.

**Status LED (optional, board-gated).** Off by default; `SIMULACRA_DETECT_LED_GPIO` unset →
no LED code compiled. Idle = off; ≥1 active confirmed threat = slow blink. Board-gated for the
same reason as the XIAO antenna GPIO (LED wiring differs across XIAO / SparkFun / C5 DevKit).

**Context safety — no flash writes in the BLE callback.** `detect_observe` runs inside
`observe_gap_event` (a NimBLE host callback). It may **log** the `THREAT` line there (that
path already uses `ESP_LOGW`) and stash the live MAC into the RAM threat record, but must
**not** touch NVS or LED GPIO from that context. On confirmation it sets a pending flag; the
**`coexist_task`** drains `detect_drain_pending()` on its next tick (right after `churn_tick`)
to do the one-time **NVS persist** (§2) and LED state update. Flash/GPIO stays confined to the
task loop; the scan callback stays fast.

**Enable / toggle / defaults.** Detection is **default-on** (`SIMULACRA_DETECT = 1`). Toggle
is exposed as an internal API — `detect_set_enabled(bool)` — for callers and the future web-UI
milestone; there is deliberately **no serial-command console** (the firmware has no
interactive input path; the monitor is read-only via pyserial). Build with
`SIMULACRA_DETECT=0` to compile it out entirely; when disabled the report callback skips
`detect_observe` (zero overhead).

| Constant | Default | Meaning |
|---|---|---|
| `SIMULACRA_DETECT` | 1 | compile-time master enable (detection default-on) |
| `DETECT_LOCATE_RSSI_DELTA` | 6 dB | min RSSI change to emit a `locate` update |
| `DETECT_LOCATE_MIN_MS` | 10000 | min interval between `locate` updates |
| `SIMULACRA_DETECT_LED_GPIO` | *(unset)* | board LED pin; unset = no LED compiled |

## Error handling

- **NVS persist of a confirmed threat fails** → log and keep the threat live in RAM (still
  alerts); retry on the next `detect_drain_pending`. Never block the decoy.
- **Candidate table full** → LRU-evict the least-recently-seen un-promoted candidate; confirmed
  threats are never evicted from the live set.
- **Detection disabled or `SIMULACRA_DETECT=0`** → report callback is a no-op; decoy unaffected.
- **Report callback context** → logging only; all flash/GPIO deferred to `coexist_task`.

## Testing & acceptance

**Host self-tests** (`CHURN_SELFTEST=1`, no radio, fake-clock) — added alongside the existing
suite (currently 89/89), all `PASS (fails=0)`:
- `test_detect_epochs` — sightings across epochs → `distinct_epochs` increments once per
  credited epoch; **CONFIRM** fires exactly at the 3rd.
- `test_detect_presence` — a 1-sighting epoch earns no credit; ≥2 credits once and only once
  per epoch.
- `test_detect_no_false_confirm` — a drive-by pattern (many epochs, 1 sighting each) never
  confirms; a device in 3 epochs but always below the sighting floor never confirms.
- `test_detect_self_exclude` — our own roster/churn active-set MACs are skipped before
  `detect_observe`.
- `test_detect_ageout` — an un-promoted candidate unseen for `AGE_OUT_EPOCHS` is evicted; LRU
  eviction under table-full holds.
- `test_detect_nvs` — confirmed-threat persist → reload round-trips (hash, epochs, first/last,
  RSSI); candidates are **not** persisted.
- `test_detect_locate_throttle` — `locate` updates gate correctly on RSSI-delta / min-interval.

**Hardware acceptance (the real test).** Flash the combined decoy (detection on) via the
`build-flash-read` skill; read serial filtered to `THREAT|epoch|reprofile|drift`. Rig:
1. A **stable-MAC beacon** carried alongside the node (a fixed iBeacon from a phone app or a
   spare board pinned to one non-rotating identity) playing the "follower."
2. Move the node across **2–3 genuinely distinct RF environments** (rooms / streets).
   Temporarily shorten `reprofile_period_ms` (as M8 acceptance did with a 5 s value; reverted
   after) so epochs are observable in minutes.
3. **Expect:** epoch increments on each real environment change (`drift > 0.45`); the carried
   beacon earns presence-credit each epoch; **`THREAT confirmed`** after the 3rd; then
   `THREAT locate` RSSI updates track it. Ambient devices seen at only **one** location must
   **not** confirm. The concurrent BLE + Wi-Fi decoy still runs, no watchdog, and self-MACs
   never self-confirm over a long idle run.

Both boards reflashed to the production decoy after, temp toggles reverted, working tree clean
— the M8 acceptance discipline.

## Out of scope (deferred)

- **Known-tracker fingerprinting (AirTag / SmartTag / Tile)** — MAC-rotating commercial tags
  are **not** caught by this behavioral layer; fingerprinting them is the separately-scoped
  **M10** detection layer.
- **Rich allowlist / "mark trusted" management** — v1 is alert-once + optional static config;
  full management is a **web-UI milestone** feature.
- **GPS / absolute location** — "location" here is an RF-drift proxy; a uniform chain-store
  environment may under-count epochs and a sparse rural move may over-trigger, mitigated (not
  guaranteed) by the balanced 3-epoch + presence bar.
- **Continuous detection RX** — detection rides the M8 re-profile windows (~15 s per 5–10 min);
  a follower must be present during those windows, which a genuine persistent follower is by
  definition. No added radio/coex load (Approach A).

## Honest ceiling (carried into the README)

M9 catches a **stable-identity device that travels with you** — a real gain against fixed-MAC
followers a simple decoy cannot see. It does **not** catch MAC-rotating commercial tags (M10),
does **not** know GPS location (RF-drift proxy only), and will occasionally flag a benign
regular (the allowlist is the release valve). Stated plainly: *M9 is a behavioral
follower-detector for non-rotating identities, not a universal tracker scanner.*
