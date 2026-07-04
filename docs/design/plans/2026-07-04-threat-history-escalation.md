# Threat History & Escalation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Raise threat confidence by recurrence — a device seen across multiple boot-sessions and location-contexts escalates NEW → RECURRING → PERSISTENT, surfaced on Vigil, for both behavioral followers and KNOWN fingerprint hits.

**Architecture:** Extend `detect.c` in place. A persistent NVS boot-session counter + three per-threat counters (`sessions_seen`, `places_seen`, `last_session`) credited on each sighting; a pure shared `threat_escalation_level()` maps counters to a 3-level ladder; the counters ride the status wire and drive Vigil's blip/row color + a DETAIL tag.

**Tech Stack:** ESP-IDF v5.5 (C5 decoy selftest, COM12) / v5.4 (CYD Vigil, COM10). Shared code in `components/simulacra_radar/`. On-target selftest `main/churn_selftest.c` (`ST_CHECK`; register `test_*` in `churn_selftest_run()`).

## Global Constraints

- **Offline sessions+places model** — no wall-clock; recurrence is boot-sessions + location-epochs, never dates.
- **`places_seen` is approximate** (epoch resets each boot); `PERSISTENT` requires session recurrence **AND** multiple places so it can't false-trigger on the approximation.
- **Law-3:** aggregate counters only, storage stays hash-only.
- **Paired builds:** the persisted + wire `detect_threat_t` bump is coordinated; bump `DETECT_THR_MAGIC` (old records drop once).
- **Persist cadence:** a session bump sets the existing `s_pending` flag so `coexist_task`'s `detect_drain_pending → detect_save_nvs` (coexist.c:248) persists a returning threat.
- Selftest build `-DCHURN_SELFTEST=1`; normal builds pass `-DCHURN_SELFTEST=0` explicitly (stale CMake-cache gotcha).
- Commits carry AI trailers → scrub before push. `main` local-only.

## File Structure

- `components/simulacra_radar/threat_escalation.h` — **new**: enum + thresholds + `threat_escalation_level` + `escalation_name` (header-only, shared).
- `main/detect.h` / `main/detect.c` — recurrence fields, session counter, `credit_recurrence`, `detect_set_session`/`detect_begin_session`, magic bump.
- `main/coexist.c` — call `detect_begin_session()` at boot.
- `components/simulacra_radar/radar_wire.h` — add `sessions_seen`/`places_seen` to the status threat element.
- `main/esp_now_link.c` — copy the two fields in `espnow_status_from_webui`.
- `components/simulacra_radar/radar_render.c` — color blips/rows by escalation + DETAIL tag.
- `main/churn_selftest.c` — tests.

---

### Task 1: shared escalation ladder

**Files:**
- Create: `components/simulacra_radar/threat_escalation.h`
- Test: `main/churn_selftest.c` (`test_escalation_ladder`)

**Interfaces:**
- Produces: `detect_escalation_t { ESCALATION_NEW=0, ESCALATION_RECURRING, ESCALATION_PERSISTENT }`;
  `threat_escalation_level(uint8_t sessions_seen, uint8_t places_seen)`; `escalation_name(detect_escalation_t)`;
  thresholds `DETECT_ESC_PERSIST_SESSIONS 3` / `DETECT_ESC_PERSIST_PLACES 2` / `DETECT_ESC_RECUR_SESSIONS 2` / `DETECT_ESC_RECUR_PLACES 3`.

- [x] **Step 1: Write the failing test** (add `#include "threat_escalation.h"` near the other sig includes; register `test_escalation_ladder();`)

```c
static void test_escalation_ladder(void)
{
    ST_CHECK(threat_escalation_level(1,1) == ESCALATION_NEW,        "esc: (1,1) NEW");
    ST_CHECK(threat_escalation_level(1,2) == ESCALATION_NEW,        "esc: (1,2) NEW");
    ST_CHECK(threat_escalation_level(2,1) == ESCALATION_RECURRING,  "esc: (2,1) RECURRING (2nd session)");
    ST_CHECK(threat_escalation_level(1,3) == ESCALATION_RECURRING,  "esc: (1,3) RECURRING (breadth)");
    ST_CHECK(threat_escalation_level(3,1) == ESCALATION_RECURRING,  "esc: (3,1) not persistent (1 place)");
    ST_CHECK(threat_escalation_level(3,2) == ESCALATION_PERSISTENT, "esc: (3,2) PERSISTENT");
    ST_CHECK(threat_escalation_level(9,9) == ESCALATION_PERSISTENT, "esc: saturated PERSISTENT");
    ST_CHECK(escalation_name(ESCALATION_PERSISTENT)[0] == 'P',      "esc: name P");
}
```

- [x] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (`threat_escalation.h` missing).

- [x] **Step 3: Create `threat_escalation.h`**

```c
#pragma once
#include <stdint.h>

typedef enum { ESCALATION_NEW = 0, ESCALATION_RECURRING, ESCALATION_PERSISTENT } detect_escalation_t;

#define DETECT_ESC_PERSIST_SESSIONS 3
#define DETECT_ESC_PERSIST_PLACES   2
#define DETECT_ESC_RECUR_SESSIONS   2
#define DETECT_ESC_RECUR_PLACES     3

static inline detect_escalation_t threat_escalation_level(uint8_t sessions_seen, uint8_t places_seen)
{
    if (sessions_seen >= DETECT_ESC_PERSIST_SESSIONS && places_seen >= DETECT_ESC_PERSIST_PLACES)
        return ESCALATION_PERSISTENT;
    if (sessions_seen >= DETECT_ESC_RECUR_SESSIONS || places_seen >= DETECT_ESC_RECUR_PLACES)
        return ESCALATION_RECURRING;
    return ESCALATION_NEW;
}
static inline const char *escalation_name(detect_escalation_t e)
{
    return e == ESCALATION_PERSISTENT ? "PERSISTENT" : e == ESCALATION_RECURRING ? "RECURRING" : "NEW";
}
```

(Header-only — no CMakeLists change needed.)

- [x] **Step 4: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS, +8.

- [x] **Step 5: Commit**

```bash
git add components/simulacra_radar/threat_escalation.h main/churn_selftest.c
git commit -m "feat(escalation): shared NEW/RECURRING/PERSISTENT ladder"
```

---

### Task 2: recurrence counters + session tracking in detect.c

**Files:**
- Modify: `main/detect.h` (fields + session API), `main/detect.c` (counter, credit, init, magic bump)
- Test: `main/churn_selftest.c` (`test_escalation_recurrence`, extend `test_detect_nvs`)

**Interfaces:**
- Produces: `detect_threat_t` gains `uint8_t sessions_seen, places_seen; uint16_t last_session;`.
  `void detect_set_session(uint16_t id);` (tests + coexist). `uint16_t detect_begin_session(void);`
  (NVS load-increment-save; returns the new id). New threats init counters to 1; a session bump sets
  `s_pending`.

- [x] **Step 1: Write the failing test** (register `test_escalation_recurrence();`)

```c
static void test_escalation_recurrence(void)
{
    detect_reset();
    detect_set_session(1);
    // Confirm a behavioral follower across 3 epochs (>=2 sightings each).
    for (uint16_t e = 1; e <= 3; e++) { detect_on_epoch_change(e);
        detect_observe(0xABCD, -50, 0x0075, e); detect_observe(0xABCD, -50, 0x0075, e); }
    detect_threat_t t;
    ST_CHECK(detect_threat_at(0, &t) && t.sessions_seen == 1 && t.places_seen == 1, "recur: fresh = 1/1");
    ST_CHECK(threat_escalation_level(t.sessions_seen, t.places_seen) == ESCALATION_NEW, "recur: fresh NEW");

    // Same session, new location-contexts -> places_seen grows.
    detect_observe(0xABCD, -50, 0x0075, 4);
    detect_observe(0xABCD, -50, 0x0075, 5);
    detect_threat_at(0, &t);
    ST_CHECK(t.sessions_seen == 1 && t.places_seen == 3, "recur: places grow within a session");
    ST_CHECK(threat_escalation_level(t.sessions_seen, t.places_seen) == ESCALATION_RECURRING, "recur: breadth RECURRING");

    // Same session + same epoch repeat -> no inflation.
    detect_observe(0xABCD, -50, 0x0075, 5);
    detect_threat_at(0, &t);
    ST_CHECK(t.places_seen == 3, "recur: same context no double count");

    // New session -> sessions_seen bumps.
    detect_set_session(2);
    detect_observe(0xABCD, -50, 0x0075, 6);
    detect_threat_at(0, &t);
    ST_CHECK(t.sessions_seen == 2, "recur: new session bumps sessions_seen");

    // KNOWN fingerprint hits escalate through the same counters.
    detect_reset(); detect_set_session(1);
    detect_note_known(0x1234, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 1);
    detect_note_known(0x1234, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 2);   // new place
    detect_set_session(2); detect_note_known(0x1234, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 3);
    detect_set_session(3); detect_note_known(0x1234, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 4);
    detect_threat_at(0, &t);
    ST_CHECK(t.sessions_seen == 3 && t.places_seen >= 2, "recur: KNOWN escalates across sessions");
    ST_CHECK(threat_escalation_level(t.sessions_seen, t.places_seen) == ESCALATION_PERSISTENT, "recur: KNOWN PERSISTENT");
}
```

Also extend `test_detect_nvs`: after confirming and before save, set `detect_set_session(4)` and re-see the threat so `sessions_seen>1`, then assert the loaded threat preserves `sessions_seen`/`places_seen` (add after the existing load assert):

```c
    // (in test_detect_nvs, after the reload) — recurrence counters survive NVS
    detect_threat_t rt; detect_threat_at(0, &rt);
    ST_CHECK(rt.sessions_seen >= 1 && rt.places_seen >= 1, "nvs: recurrence counters restored");
```

- [x] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (`detect_set_session`/fields missing).

- [x] **Step 3: Extend `detect.h`**

Add the fields to `detect_threat_t` (after `confidence`):

```c
    uint8_t  sessions_seen;   // distinct boot-sessions observed in (saturating)
    uint8_t  places_seen;     // distinct location-contexts observed in (saturating)
    uint16_t last_session;    // session id of the last sighting
```

Add the session API declarations (near `detect_load_salt`):

```c
void     detect_set_session(uint16_t id);   // set current boot-session id (tests + coexist)
uint16_t detect_begin_session(void);        // NVS load-increment-save the boot counter; sets + returns it
```

- [x] **Step 4: Implement in `detect.c`**

Bump the magic (struct grew again): `#define DETECT_THR_MAGIC 0x4D394433u   // 'M9D3'`.

Add the session state + API + credit helper (near the top, after the statics):

```c
static uint16_t s_session;

void detect_set_session(uint16_t id) { s_session = id; }

// Bump recurrence counters for an already-recorded threat. A session bump marks the threat pending
// so the coordinator persists a returning device. Call BEFORE updating t->last_epoch.
static void credit_recurrence(detect_threat_t *t, uint16_t epoch)
{
    if (t->last_session != s_session) {
        if (t->sessions_seen < 255) t->sessions_seen++;
        t->last_session = s_session;
        s_pending = true; s_pending_threat = *t;
    }
    if (t->last_epoch != epoch && t->places_seen < 255) t->places_seen++;
}
```

In `detect_reset`, add `s_session = 0;`.

In `detect_observe` existing-threat branch, insert the credit before `last_epoch` is set:

```c
    detect_threat_t *t = threat_find(hash);
    if (t) {
        if (rssi > t->best_rssi) t->best_rssi = rssi;
        credit_recurrence(t, epoch);
        t->last_epoch = epoch;
        return DETECT_KNOWN;
    }
```

In `promote()`, after the `kind`/class clears, initialize the counters:

```c
    t->kind = DETECT_KIND_FOLLOWER; t->class_id = 0; t->category = 0; t->confidence = 0;
    t->sessions_seen = 1; t->places_seen = 1; t->last_session = s_session;
```

In `detect_note_known`, the existing-row branch:

```c
    if (t) {
        if (rssi > t->best_rssi) t->best_rssi = rssi;
        credit_recurrence(t, epoch);
        t->last_epoch = epoch;
        return DETECT_KNOWN;
    }
```

and the new-row path, after the KNOWN fields are set:

```c
    t->kind = DETECT_KIND_KNOWN; t->class_id = class_id; t->category = category; t->confidence = confidence;
    t->sessions_seen = 1; t->places_seen = 1; t->last_session = s_session;
```

Add `detect_begin_session` in the persistence section (mirror `detect_load_salt`), with the NVS key
alongside the others (`DETECT_KEY_SALT`/`DETECT_KEY_THR`):

```c
#define DETECT_KEY_SESS  "detect_sess"
uint16_t detect_begin_session(void)
{
    nvs_handle_t h; uint32_t v = 0;
    if (nvs_open(DETECT_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, DETECT_KEY_SESS, &v);        // NOT_FOUND -> v stays 0
        v++;
        nvs_set_u32(h, DETECT_KEY_SESS, v);
        nvs_commit(h);
        nvs_close(h);
    } else {
        v = 1;
    }
    s_session = (uint16_t)v;
    return s_session;
}
```

Add `#include "threat_escalation.h"` is not required in detect.c (the level function is used by callers/UI, not the core), but include it if you want the enum there. Not needed for this task.

- [x] **Step 5: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS. Existing detect tests still green (they don't set a session; `s_session` starts 0, `detect_reset` keeps it 0, new threats init to 1 — behavior unchanged for them).

- [x] **Step 6: Commit**

```bash
git add main/detect.h main/detect.c main/churn_selftest.c
git commit -m "feat(escalation): per-threat recurrence counters + boot-session tracking"
```

---

### Task 3: session boot-init + status wire

**Files:**
- Modify: `main/coexist.c` (call `detect_begin_session`), `components/simulacra_radar/radar_wire.h` (wire fields), `main/esp_now_link.c` (copy fields)
- Test: `main/churn_selftest.c` (extend `test_espnow_convert`)

**Interfaces:**
- Produces: `radar_wire_status_t.threats[i]` gains `uint8_t sessions_seen, places_seen;` after `confidence`.

- [x] **Step 1: Extend the failing test** — in `test_espnow_convert`, set + assert the new fields:

```c
    w.threats[0].sessions_seen = 3; w.threats[0].places_seen = 2;
    // ... after espnow_status_from_webui(&r, &w):
    ST_CHECK(r.threats[0].sessions_seen == 3 && r.threats[0].places_seen == 2, "convert: recurrence carried");
```

- [x] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (fields missing).

- [x] **Step 3: Add the wire fields** — in `radar_wire.h`, the threats element becomes:

```c
    struct __attribute__((packed)) {
        uint32_t hash; uint16_t vendor; uint8_t epochs; int8_t best_rssi;
        uint16_t first_epoch, last_epoch;
        uint8_t kind, class_id, category, confidence;
        uint8_t sessions_seen, places_seen;         // recurrence (escalation)
    } threats[RADAR_MAX_THREATS];
```

In `espnow_status_from_webui` (`esp_now_link.c`), add to the per-threat copy loop:

```c
        out->threats[i].sessions_seen = in->threats[i].sessions_seen;
        out->threats[i].places_seen = in->threats[i].places_seen;
```

In `coexist.c` `coexist_start`, add the session init alongside the other detect boot calls (after `detect_load_salt()` / before/after `detect_load_nvs()`):

```c
    detect_begin_session();                       // escalation: bump + load the persistent boot-session id
```

- [x] **Step 4: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS. Frame size stays < 218 B (~183 B). Also build the decoy to confirm coexist wiring: `idf.py -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1 -p COM12 build`.

- [x] **Step 5: Commit**

```bash
git add main/coexist.c components/simulacra_radar/radar_wire.h main/esp_now_link.c main/churn_selftest.c
git commit -m "feat(escalation): boot-session init + recurrence counters on the status wire"
```

---

### Task 4: Vigil escalation display

**Files:**
- Modify: `components/simulacra_radar/radar_render.c`
- Test: build + bench

**Interfaces:**
- Consumes: `threat_escalation_level`, `escalation_name`, wire `sessions_seen`/`places_seen`.

- [x] **Step 1: Color helper + includes** — in `radar_render.c` add `#include "threat_escalation.h"` and:

```c
static uint16_t escalation_color(detect_escalation_t e){
    return e==ESCALATION_PERSISTENT ? 0xF800   // red
         : e==ESCALATION_RECURRING  ? 0xFD20    // orange
                                    : 0xFFE0;   // yellow (NEW)
}
```

- [x] **Step 2: Color radar blips by escalation** — replace the blip color line in `draw_radar`:

```c
        detect_escalation_t e = threat_escalation_level(st->threats[i].sessions_seen, st->threats[i].places_seen);
        radar_gfx_fill_rect(g,x-2,y-2,5,5,escalation_color(e)); }
```

(Removes the prior KNOWN-amber / `threat_color(epochs)` blip logic; escalation color now applies uniformly. The KNOWN vs follower distinction stays in the DETAIL text.)

- [x] **Step 3: Tag + color DETAIL rows** — rewrite the `draw_detail` per-threat loop body:

```c
    for(uint8_t i=0;i<st->threat_count;i++){ char r[48];
        detect_escalation_t e = threat_escalation_level(st->threats[i].sessions_seen, st->threats[i].places_seen);
        char tag = escalation_name(e)[0];   // N / R / P
        if(st->threats[i].kind==DETECT_KIND_KNOWN){
            const char *q = st->threats[i].confidence>=80 ? "likely" : "possible";
            snprintf(r,sizeof r,"%s %s %ddB %c%u/%u",sig_class_name(st->threats[i].class_id),q,
                     (int)st->threats[i].best_rssi,tag,
                     (unsigned)st->threats[i].sessions_seen,(unsigned)st->threats[i].places_seen);
        } else {
            snprintf(r,sizeof r,"%08lx %ddB %c%u/%u",(unsigned long)st->threats[i].hash,
                     (int)st->threats[i].best_rssi,tag,
                     (unsigned)st->threats[i].sessions_seen,(unsigned)st->threats[i].places_seen);
        }
        radar_gfx_text(g,6,30+i*18,r,escalation_color(e)); } }
```

- [x] **Step 4: Build Vigil** — from `cyd/`: `idf.py build`; clean.

- [x] **Step 5: Commit**

```bash
git add components/simulacra_radar/radar_render.c
git commit -m "feat(vigil): escalation-colored threat blips + DETAIL level/counts tag"
```

---

### Task 5: bench verification + finish

**Files:** none (bench).

- [x] **Step 1: Flash both** — Vigil (from `cyd/`): `idf.py -p COM10 flash`. Decoy: `idf.py -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1 -p COM12 flash`.

- [x] **Step 2: Session counter persists** — read the decoy across two reboots; confirm no crash and that a confirmed threat's `sessions_seen` increments on the second boot (watch a `THREAT confirmed`/`status rx` line, or add a temporary log if needed). Because cross-session recurrence is slow to force, this is best-effort — the pure logic is the primary verification (Tasks 1–2 selftest).

- [x] **Step 3: Display** — with at least one confirmed threat present, verify the CYD DETAIL row shows the `N/R/P` tag + `Ns/Mp` counts and the blip/row color matches (yellow/orange/red). Serial-verify via `status rx` if the screen isn't visible.

- [x] **Step 4: Regression** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor` → `SELFTEST result: PASS`. Restore the display-paired decoy build after.

- [x] **Step 5: Finish** — mark checkboxes; use superpowers:finishing-a-development-branch (verify selftest PASS, offer merge options; established pattern = `--no-ff` merge to local `main`).

---

## Self-Review

**Spec coverage:** session counter (T2) · recurrence fields + crediting (T2) · new-threat init (T2) · persist-on-session-bump via `s_pending` (T2, consumed by coexist:248) · shared 3-level ladder (T1) · boot init (T3) · status wire (T3) · Vigil color + tag (T4) · KNOWN + follower share the machinery (T2 test) · bench + regression (T5). All spec sections mapped.

**Type consistency:** `detect_escalation_t`, `threat_escalation_level(sessions_seen, places_seen)`, `escalation_name`, `detect_set_session`/`detect_begin_session`, `credit_recurrence`, and the `sessions_seen`/`places_seen`/`last_session` field names are used identically across tasks and match `detect_threat_t` ↔ the wire element.

**Notes:** `threat_escalation.h` is header-only (no CMakeLists change). `DETECT_THR_MAGIC` bump to `M9D3`. Existing detect tests are unaffected (they don't set a session; new threats init counters to 1).

## Execution Record (2026-07-04)

All 5 tasks implemented inline; selftest green on C5 COM12, clean-boot verified on C5+CYD.

- **Selftest:** PASS, fails=0, +11 checks (ladder table 8, recurrence 2 groups, wire-convert 1, nvs 1). Recurrence arithmetic traced and confirmed: fresh follower 1/1 NEW → places grow within session → RECURRING → new session bumps `sessions_seen`; KNOWN fingerprint reaches 3 sessions / ≥2 places → PERSISTENT.
- **Clean boot both boards:** decoy `NimBLE host synced` (no crash from the new `detect_sess` NVS key or the M9D3 format bump); CYD `learndb/sigdb loaded` + `panel up` (no crash from the wider wire/threat struct). No boot loop.
- **NOT machine-verified (best-effort, per plan):** the cross-session `sessions_seen` climb over real reboots and the on-screen escalation color/tag — both need a real persistent threat present across reboots plus a look at the CYD. Pure logic is the primary verification (selftest). Both boards left on the escalation build.
- Commits: tasks 1–3 (`aa6d6fe`), task 4 (`c3a5ec0`).
