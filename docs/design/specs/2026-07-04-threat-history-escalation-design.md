# Threat History & Escalation — Design Spec

**Date:** 2026-07-04
**Status:** Approved for planning
**Feature line:** Threat history & escalating confidence (see memory `feature-backlog`). Builds directly
on the M9 detector (`detect.c` + coexist location-epochs) and the M10 fingerprint layer
(`detect-surveillance-fingerprints`, shipped slice 1).

## Goal

Raise confidence in a threat by its **recurrence**: a device seen across multiple power-on sessions
and multiple location-contexts is a high-confidence stalker; a one-off co-presence is not. Turns a
confirmed follower *or* a returning fingerprinted tracker (e.g. an AirTag) from "present" into
"following you," and directly attacks the false-positive caveat the project admits. Detection/awareness
only.

## Scope

**In scope:**
- A persistent per-install **boot-session counter** (NVS).
- Per-threat **recurrence counters** (`sessions_seen`, `places_seen`, `last_session`) on the existing
  `detect_threat_t`, credited on every sighting of an already-recorded threat.
- A pure **3-level escalation ladder** (`NEW` / `RECURRING` / `PERSISTENT`) derived from the counters,
  shared so both the decoy and Vigil compute it identically.
- Surfacing on Vigil: escalation-driven blip/row color + a DETAIL tag showing the level and counts.
- Applies uniformly to behavioral followers and KNOWN fingerprint hits (same threat table).

**Out of scope (deliberately):**
- Wall-clock / calendar "days" (no reliable RTC or NTP on the offline decoy — see Constraints). The
  model is **sessions + places**, not dates.
- On-body haptic/buzzer alerting on the level (separate `feature-backlog` item; this spec only makes
  the level available for it to consume later).
- Stable cross-reboot place IDs (the location-epoch counter resets each boot — see Constraints).

## Global Constraints

- **Offline-first, no wall-clock.** The build has no RTC and no reliable NTP (the boot Wi-Fi window is
  a SoftAP with no internet; STA probe-injection isn't associated). Recurrence is measured in
  **boot-sessions** and **location-epochs**, never dates.
- **`s_epoch` resets to 0 each boot** and is not a stable place identifier, so `places_seen` is an
  *approximation* of distinct location-contexts over time (a reboot can bump it by one). The ladder
  compensates: `PERSISTENT` requires real session recurrence **and** multiple places, so `places_seen`
  alone can never reach "stalker."
- **Law-3 / data discipline:** counters are aggregate integers; storage stays hash-only, no raw
  identity, exactly as `detect.c` is today.
- **Paired builds:** decoy + Vigil are flashed together, so the persisted + wire `detect_threat_t`
  format bump is coordinated. Bump `DETECT_THR_MAGIC` (old records drop once and re-confirm).
- Commits carry AI trailers → scrub before any push. `main` is local-only.
- Selftest build = `-DCHURN_SELFTEST=1`; normal builds pass `-DCHURN_SELFTEST=0` explicitly (stale
  CMake-cache gotcha).

## Architecture

Approach 1 (extend `detect.c` in place). The recurrence model is a natural extension of the
location-epoch + NVS persistence the detector already has. No new module, one threat table, both
threat kinds escalate through the same counters.

- **Session counter** — a persistent `u32` in NVS (`splinter`/`detect_sess`), incremented once per
  boot (mirrors `detect_load_salt`). Loaded into a static `s_session`; a `detect_set_session(id)` seam
  keeps the core testable without NVS/radio.
- **Crediting** — on every sighting of an already-recorded threat (the behavioral `detect_observe`
  re-hit path and the fingerprint `detect_note_known` re-hit path), a helper bumps `sessions_seen` when
  the session changed and `places_seen` when the location-epoch changed. New threats initialize the
  counters to 1.
- **Ladder** — a pure, shared `threat_escalation_level(sessions_seen, places_seen)` maps the two
  counters to the enum. Shared so Vigil derives the identical level from the wire counters.
- **Surface** — the two counters ride the status wire; Vigil colors blips/rows by level and tags the
  DETAIL row with the level + counts.

## Data Model

```c
typedef struct {
    uint32_t hash; uint16_t vendor;
    uint8_t  epochs; int8_t best_rssi;
    uint16_t first_epoch, last_epoch;
    uint8_t  kind, class_id, category, confidence;   // (existing: M9 kind + M10 fingerprint)
    uint8_t  sessions_seen;   // distinct boot-sessions observed in (saturating at 255)
    uint8_t  places_seen;     // distinct location-contexts observed in (saturating at 255)
    uint16_t last_session;    // session id of the last sighting (detect a new session)
} detect_threat_t;            // 16 -> 20 B
```

Shared escalation header (`components/simulacra_radar/threat_escalation.h`):

```c
typedef enum { ESCALATION_NEW = 0, ESCALATION_RECURRING, ESCALATION_PERSISTENT } detect_escalation_t;

#define DETECT_ESC_PERSIST_SESSIONS 3
#define DETECT_ESC_PERSIST_PLACES   2
#define DETECT_ESC_RECUR_SESSIONS   2
#define DETECT_ESC_RECUR_PLACES     3

static inline detect_escalation_t threat_escalation_level(uint8_t sessions_seen, uint8_t places_seen) {
    if (sessions_seen >= DETECT_ESC_PERSIST_SESSIONS && places_seen >= DETECT_ESC_PERSIST_PLACES)
        return ESCALATION_PERSISTENT;
    if (sessions_seen >= DETECT_ESC_RECUR_SESSIONS || places_seen >= DETECT_ESC_RECUR_PLACES)
        return ESCALATION_RECURRING;
    return ESCALATION_NEW;
}
static inline const char *escalation_name(detect_escalation_t e) {
    return e == ESCALATION_PERSISTENT ? "PERSISTENT" : e == ESCALATION_RECURRING ? "RECURRING" : "NEW";
}
```

## Components

`detect.h` / `detect.c`:
- Add the three fields to `detect_threat_t`; bump `DETECT_THR_MAGIC`.
- `void detect_set_session(uint16_t id);` — set the current session (tests + coexist).
- `uint16_t detect_begin_session(void);` — NVS load-increment-save the boot counter, set `s_session`,
  return it (coexist calls this at boot; mirrors `detect_load_salt`).
- `static void credit_recurrence(detect_threat_t *t, uint16_t epoch);` — bump session/place counters.
  Called in the `detect_observe` existing-threat branch and the `detect_note_known` existing branch,
  before `last_epoch`/`last_session` are updated; new threats (in `promote()` and the new-row path of
  `detect_note_known`) initialize `sessions_seen = places_seen = 1, last_session = s_session`.
- Reuse `threat_escalation_level()` from the shared header (include it).

`coexist.c`:
- Call `detect_begin_session()` at startup (alongside `detect_load_salt()` / `detect_load_nvs()`).

`components/simulacra_radar/`:
- New `threat_escalation.h` (enum + thresholds + `threat_escalation_level` + `escalation_name`).
- `radar_wire.h`: add `sessions_seen`, `places_seen` to the per-threat status element.
- `radar_render.c`: color blips + DETAIL rows by `threat_escalation_level(...)`; tag the DETAIL row
  with the level name + `Ns/Mp`.

`main/esp_now_link.c` (`espnow_status_from_webui`) + `main/webui.*`:
- `webui_status_t.threats[]` is already `detect_threat_t`, so the new fields flow into the snapshot via
  `detect_threat_at` for free. `espnow_status_from_webui` copies `sessions_seen`/`places_seen` into the
  wire element.

## Data Flow

1. **Boot:** `coexist` calls `detect_begin_session()` → `s_session` = prior + 1 (persisted). Confirmed
   threats restored from NVS carry their `sessions_seen`/`places_seen`/`last_session` from before.
2. **Sighting of a recorded threat:** `credit_recurrence` bumps `sessions_seen` when
   `last_session != s_session` (first sighting this power-on) and `places_seen` when
   `last_epoch != epoch` (moved to a new context).
3. **Level:** `threat_escalation_level(sessions_seen, places_seen)` → NEW / RECURRING / PERSISTENT.
4. **Persist:** the debounced `detect_save_nvs` (already wired in coexist on new confirmations) carries
   the updated counters. *(See Error/Edge Handling for the persist-cadence note.)*
5. **Surface:** counters ride the status frame; Vigil colors by level and tags the DETAIL row.

## Error / Edge Handling

- **Counter saturation:** `sessions_seen`/`places_seen` saturate at 255; the ladder only cares about
  small thresholds, so saturation is cosmetic.
- **Format bump:** the grown `detect_threat_t` changes the NVS blob size, so old records fail the
  length/magic check and are dropped once (re-confirm quickly) — same one-time effect as the M10 bump.
- **Persist cadence:** today `detect_save_nvs` runs only when `detect_drain_pending` fires (a *new*
  confirmation). Recurrence updates on an already-confirmed threat would otherwise not be persisted
  until the next new confirmation. To keep cross-session recurrence durable, the coordinator must
  persist when a recurrence bump raises `sessions_seen` (a returning threat is worth a write). The plan
  will mark a "recurrence changed" pending flag (reuse the existing `s_pending` hand-off, extended to
  fire on a session bump) so a returning threat is saved. Bounded writes (a session bump happens at
  most once per boot per threat).
- **Session counter wrap:** `u32` NVS store, `uint16_t s_session` — wraps after 65535 boots; harmless
  (only equality vs `last_session` matters).
- **places_seen reboot approximation:** documented in Constraints; the `PERSISTENT` AND-gate prevents
  it from alone reaching stalker level.

## Testing

Pure units in `main/churn_selftest.c` (on-target PASS/FAIL, no radio):
- **Ladder table:** `threat_escalation_level` for representative `(sessions, places)` pairs →
  NEW/RECURRING/PERSISTENT boundaries (e.g. (1,1)=NEW, (2,1)=RECURRING, (1,3)=RECURRING,
  (3,2)=PERSISTENT, (3,1)=RECURRING).
- **Crediting across sessions:** `detect_set_session(1)`, confirm a follower → `sessions_seen==1`,
  level NEW. Re-see across distinct epochs → `places_seen` grows. `detect_set_session(2)`, re-see →
  `sessions_seen==2` (RECURRING). `detect_set_session(3)` + a place bump → PERSISTENT.
- **Same-session no double count:** repeated sightings in one session/epoch don't inflate the counters.
- **KNOWN escalates too:** `detect_note_known` re-hits across `detect_set_session` bumps raise the
  level identically.
- **NVS round-trip:** `detect_save_nvs`/`detect_load_nvs` preserve the new counters.

Bench (best-effort): reboot the decoy a few times with a persistent ambient device present; watch
`sessions_seen` climb and the Vigil level/color rise. Full cross-session recurrence is slow to force,
so the pure logic is the primary verification (same posture as the rest of the detector).

## Future (not this spec)

- On-body haptic/buzzer alerting keyed to `ESCALATION_PERSISTENT` (separate backlog item; consumes this
  level).
- Wall-clock timestamps ("first seen 3 days ago") if a reliable time source (RTC/NTP) is ever added.
- Stable place fingerprints (a real location signature) to replace the epoch-reset approximation.
