# M5 — Observe → Model (design spec)

**Status:** Approved (2026-06-25). Supersedes nothing; extends the M3/M4 synthetic-population engine.

**Milestone goal (from the handoff):** profile the ambient RF environment into *distributions*,
with the strictest data discipline of any milestone. Extract features only, hash-and-drop
identifiers at capture time, aggregate into a small model, and persist the **model** — never raw
captures. M6 later samples this model to generate the population; M5 only builds and verifies it.

## Scope (decided)

- **BLE-only.** Wi-Fi promiscuous profiling is deferred to land with M7/M8 (where 2.4 GHz
  software coexistence is the explicit job). M5 stays on one radio.
- **Dedicated observe mode.** A new compile-time build mode that *only* scans + models; it never
  advertises, so the device can never hear its own decoys (no model self-poisoning). The default
  shipped firmware is unchanged (still the M4 decoy population). Running decoy + observe together
  is M8's coexistence work, not M5's.

## Invariants this milestone must honor (handoff principles)

1. **Aggregates only** — the model stores distributions/counts, never per-device identifiers.
2. **Hash-and-drop at capture** — MAC/name/payload are reduced to features and dropped *before*
   anything is persisted; identifiers never reach NVS.
3. **Non-connectable / popup-safe** — unchanged (observe mode doesn't advertise at all).
4. **Population-match readiness** — the model carries the counts M6 needs to size the active set.
5. **Raw-capture dumps live behind a compile-time debug flag that does not ship.**

## Build modes (three, mutually exclusive at compile time)

`simulacra_main.c` gains a second flag alongside the existing `CHURN_SELFTEST`:

| Flag | Behavior |
|------|----------|
| `SIMULACRA_OBSERVE=1` | Live observe loop: passive scan → model → NVS persist → serial dump. Radio never advertises. |
| `CHURN_SELFTEST=1` | All pure-logic on-target self-tests (M3 churn + M4 templates + **M5 aggregation**), radio idle. |
| both `0` (default, shipped) | The M4 decoy population (unchanged). |

Precedence in `simulacra_task`: `SIMULACRA_OBSERVE` > `CHURN_SELFTEST` > decoy.

## Module layout (new, focused files < 500 lines each)

- **`rf_model.{h,c}`** — the `rf_model_t` schema; pure aggregation helpers (fold a single
  observation into the model); NVS save/load (versioned blob); and `rf_model_dump()` that prints
  the model over serial for inspection. No NimBLE dependency → unit-testable off-radio.
- **`observe.{h,c}`** — owns the NimBLE passive scan (`ble_gap_disc`), the `BLE_GAP_EVENT_DISC`
  callback, feature extraction, the **ephemeral dedup table**, sweep timing, and the persist/dump
  cadence. Calls into `rf_model`.
- **`observe_selftest`** — added to the existing looped self-test runner: feeds synthetic
  observations through `rf_model` aggregation and asserts the data-discipline properties.

## Capture path & data discipline (the heart of M5)

For each `BLE_GAP_EVENT_DISC` report:

1. **Extract features only:** company ID (from the mfg-data AD via `ble_hs_adv_parse_fields`),
   `rssi` (int8), `event_type` (PDU type), and which AD types are present.
2. **Salted hash for dedup:** compute `h = fnv1a32(boot_salt, addr.val[0..5])`. `boot_salt` is a
   random 32-bit value generated once per boot (`esp_random()`), **never stored**. The raw `addr`,
   any name, and the payload are **dropped immediately** — never copied into persistent state.
3. **Ephemeral dedup table** (RAM only, fixed capacity ~256, wiped every sweep): holds
   `{h, first_seen_ms, last_seen_ms}`. On a *repeat* `h`, an interval sample
   `(now - last_seen)` is folded into that vendor's interval histogram; `last_seen` updated.
   On a *new* `h`, it's an arrival.
4. **Only aggregates persist.** Nothing reversible to a device leaves RAM.

The boot salt makes even the in-RAM hashes non-linkable across boots/devices — the self-test
asserts two different salts hash the same MAC to different values, proving the stored model carries
no stable identifier.

## The model schema (`rf_model_t`) — bounded, all counts

| Component | Shape | M6 use |
|-----------|-------|--------|
| Vendor mix | fixed table of 24 × `{ uint16 company_id, uint32 count, uint16 itvl_bins[7] }` + `uint32 other_count` | weight synthesized vendor mix; per-vendor interval band |
| Interval bins | per vendor slot, 7 **log-spaced** bins: `<50, 50–100, 100–200, 200–500, 500–1000, 1000–2000, >2000 ms` | reproduce observed cadence |
| RSSI spread | global `uint32 rssi_bins[8]` over −100…−20 dBm | TX/RSSI dithering |
| PDU type | global `uint32 pdu_bins[5]` (ADV_IND / NONCONN_IND / SCAN_IND / DIR_IND / SCAN_RSP) | realism |
| Population | `float pop_ewma` (distinct devices/sweep, EWMA-smoothed) | active-set sizing |
| Arrival rate | `float arrival_per_min` (new distinct hashes / window, EWMA) | churn cadence |
| Header | `uint32 magic`, `uint16 version`, `uint32 sweeps`, `uint32 total_obs` | NVS forward-compat |

Total < ~1 KB. **By construction there is no `uint8 addr[6]`, no name buffer, no payload buffer** —
the data-discipline guarantee is structural, not just procedural. We deliberately capture
*scanner-observed* intervals (which run inflated vs. nominal, as M4 acceptance showed) — that is
exactly what M6 should reproduce for believability.

Vendor-table policy: on observation, find the slot by `company_id`; if absent and a slot is free,
claim it; else increment `other_count` (its interval folds into a shared "other" path, not a
per-vendor bin). Simple, bounded, faithful to the dominant vendors.

## Persistence & cadence

- **NVS** (the XIAO C6 has no SD slot; `nvs_flash_init()` already runs in `app_main`). The model is
  a single versioned blob under namespace `splinter`, key `rf_model`. On boot, load if present
  (and version matches) else start empty.
- **Sweep** = a fixed observation window (default **60 s**). At each sweep end: fold the window's
  ephemeral stats (population, arrivals) into the EWMA fields and **wipe the ephemeral table**.
- **Persist on a slow cadence** — every **5 sweeps** (~5 min) write the blob to NVS. At ~1 KB every
  5 min, NVS wear is negligible for the device's lifetime.

## Verification & acceptance

- **On-target self-test** (in the `CHURN_SELFTEST` build, looped `ST_CHECK`): feed a scripted set of
  synthetic observations through `rf_model` aggregation and assert:
  - vendor counts, per-vendor interval bins, RSSI bins, and PDU bins update as expected;
  - distinct-device population reflects the number of *distinct* hashes (repeats don't inflate it);
  - the ephemeral table is empty after a sweep boundary;
  - **structural no-identifier check:** the persisted model round-trips through save/load and the
    same-MAC-different-salt hashes differ (no stable identifier retained).
- **Serial model-dump** on real ambient RF: `SIMULACRA_OBSERVE` build, let it run a few sweeps near
  normal BLE traffic, read `rf_model_dump()` over the C6 serial port. Acceptance = the dump is all
  distributions/counts with **no recoverable per-device data**, and population/vendor mix look
  plausible for the room. This is the handoff's M5 acceptance, done on hardware.
- **Each milestone leaves a flashable working device:** the *default* build is still the M4 decoy;
  observe is an opt-in build. Flipping `SIMULACRA_OBSERVE` back to 0 ships the decoy unchanged.

## Out of scope for M5 (explicit)

- Wi-Fi probe-request observation (M7/M8).
- Feeding the model into generation / population-match (M6).
- Concurrent advertise + observe and any duty-cycling (M8).
- Own-MAC self-exclusion (only needed once advertise + observe run together — M6/M8).

## Risks / watch-items

- **NimBLE scan + host task:** observe runs in its own task after host sync, same pattern as the
  decoy loop; `ble_gap_disc` with `duration_ms = BLE_HS_FOREVER` and a restart-on-complete guard.
- **Ephemeral-table saturation** in dense RF: cap at 256, flag `saturated` in the sweep so the
  population estimate is reported as a lower bound rather than silently truncating.
- **Serial-read window vs. sweep length:** the 60 s sweep is longer than the ~13 s reader; observe
  mode prints a short per-sweep heartbeat so the reader always catches activity, and a full dump
  every persist. The plan adapts the reader duration for the hardware acceptance step.
