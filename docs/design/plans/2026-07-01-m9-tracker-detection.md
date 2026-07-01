# M9 — Passive Tracker / Follower Detection ("Threat Radar") Implementation Plan

**Goal:** While the device decoys, passively detect a **stable-identity device that follows you** across multiple distinct RF environments and raise a serial (optionally LED) threat alert so the user can locate it.

**Architecture:** Approach A (piggyback). A pure, host-testable core (`detect.c`) owns the candidate table, location-epoch accounting, and the 3-strike threshold. It is fed by two events the M8 pipeline already produces: a per-report tap in `observe_gap_event` (raw MAC + RSSI + company, before hashing) via a registered callback, and a location-epoch bump computed from the existing M8 `drift_score` in `coexist_reprofile`. A thin wiring adapter in `coexist.c` owns everything impure (per-install salt, self-exclusion, MAC→hash, epoch injection, serial/LED output, deferred NVS persist). Zero new radio or coexistence load.

**Tech Stack:** ESP-IDF v5.5 (Ward/C5) / v5.4 (Shade/C6), NimBLE ext-adv + observer, ESP-IDF SW coexistence, NVS blob storage, C. Verified by the looped on-target self-test (pure logic, radio idle) + hardware acceptance with a carried stable-MAC beacon.

**Spec:** `docs/design/specs/2026-07-01-m9-tracker-detection-design.md`

## Global Constraints

- **Data discipline:** the `rf_model` and observe dedup pipeline stay untouched (aggregates-only, per-boot salt, wiped each sweep). M9's detector adds **one documented departure**: a **per-install** salt (persisted, distinct from observe's per-boot salt) so a device's hash is stable across sweeps/reboots, and retention of a **live raw MAC for confirmed threats only** (emitted in the alert; never persisted, never held for unconfirmed candidates). The pure core `detect.c` only ever sees a hash — no raw identifier enters its decision logic or the NVS record.
- **Detection thresholds are persona-shared** (the balanced decision). Only the re-profile cadence differs (falls out of M8: Ward 10 min / Shade 5 min).
- **Personas:** behavior differentiates by `#if CONFIG_IDF_TARGET_ESP32C5` (Ward: C5, fixed/vehicle) vs C6 (Shade: portable EDC).
- **Toolchain:** Ward/C5 → ESP-IDF **v5.5** (`~\esp\v5.5\esp-idf`, target esp32c5, **COM12**), Python 3.12 forced on PATH. Shade/C6 → ESP-IDF **v5.4** (`~\esp\v5.4\esp-idf`, target esp32c6, **COM11**); XIAO C6 needs `-DSIMULACRA_BOARD_XIAO_C6=1`.
- **Project rule:** keep files **< 500 lines**; typed interfaces; TDD (host self-test first, radio idle).
- **Default build (all `SIMULACRA_*` / `CHURN_SELFTEST` = 0) is the shipped decoy** — after M9 it is the combined coexist decoy with detection on (`SIMULACRA_DETECT` defaults 1).
- **Non-derivative:** concepts reimplemented from scratch; no code copied from any sibling fork.

### Build modes after M9

| Flag (all default 0 except `SIMULACRA_DETECT`=1) | Behavior |
|---|---|
| *(none set)* | **Combined coexist decoy + Threat Radar (new default)** |
| `SIMULACRA_DETECT=0` | Combined decoy, detection compiled out (zero overhead) |
| `SIMULACRA_PROBE` / `SIMULACRA_SNIFF` / `SIMULACRA_OBSERVE` | single-stack dev modes (unchanged) |
| `CHURN_SELFTEST` | On-target host-logic self-test (radio idle) |

### Reusable build / flash / read recipes

> **Target governs `sdkconfig.defaults.*`.** Switch personas with `Remove-Item build,sdkconfig -Recurse -Force; idf.py set-target <esp32c5|esp32c6>` before the first build for that persona. Workspace is currently targeted **esp32c6** (Shade).

**SELFTEST run (either persona):** temporarily set `#define CHURN_SELFTEST 1` in `main/simulacra_main.c`, build+flash, read serial for the `SELFTEST: PASS (n/n)` line, then **revert to 0** before the production commit. The self-test runs radio-idle and loops the PASS/FAIL line.

**BUILD/FLASH/READ-C6** (Shade, PowerShell — XIAO on COM11):
```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$env:IDF_PATH='~\esp\v5.4\esp-idf'; . $env:IDF_PATH\export.ps1 *> $null
idf.py -DSIMULACRA_BOARD_XIAO_C6=1 build
idf.py -p COM11 flash
# read looped serial (no chip reset) via the project pyserial reader, PORT=COM11
```
**BUILD/FLASH/READ-C5** (Ward): same but `IDF_PATH=...\v5.5\esp-idf`, `idf.py set-target esp32c5` once, `-p COM12`, no XIAO flag.

Expected self-test line after M9: `SELFTEST: PASS (N/N)` with N = 89 + the seven new `test_detect_*` assertions groups.

---

## File Structure

**New**
- `main/detect.h` — public interface: tunable macros, `detect_result_t`, the pure core functions, confirmed-threat accessors, NVS persist/load, per-install salt. One responsibility: the detection decision model.
- `main/detect.c` — the pure core + its own NVS persist/load (mirrors how `rf_model.c` owns its NVS blob). Candidate table, epoch/strike logic, locate-due helper, self-exclusion helper. No radio, no GPIO; the decision path calls no NVS (persist is a separate function drained by the coordinator).

**Modified**
- `main/observe.h` / `main/observe.c` — add a registered report callback fired from `observe_gap_event` with `(mac, rssi, company)` before the MAC is hashed away. Observe gains no dependency on `detect`.
- `main/coexist.h` / `main/coexist.c` — own the `uint16_t` location-epoch counter; bump it in `coexist_reprofile` after `drift_score`; host the wiring adapter (register observe callback, build the self-exclusion set from churn, hash with the per-install salt, inject the current epoch, call `detect_observe`, emit `THREAT` serial lines, throttle `locate`); drain `detect_drain_pending()` each `coexist_task` tick for the deferred NVS persist + LED.
- `main/simulacra_main.c` — in the default decoy path, initialize the detector (load salt, restore persisted threats, register the observe callback, honor `SIMULACRA_DETECT`).
- `main/CMakeLists.txt` — add `detect.c` to SRCS.
- `main/churn_selftest.c` — add the `test_detect_*` functions and their calls; `#include "detect.h"`.

---

## Task 1: Detection core — candidate table, epochs, 3-strike confirmation

**Files:**
- Create: `main/detect.h`
- Create: `main/detect.c`
- Modify: `main/CMakeLists.txt:2` (add `detect.c` to SRCS)
- Modify: `main/churn_selftest.c` (add `#include "detect.h"`, three test fns + calls)

**Interfaces:**
- Produces:
  - `typedef enum { DETECT_NONE = 0, DETECT_CONFIRM, DETECT_KNOWN } detect_result_t;`
  - `void detect_reset(void);`
  - `void detect_set_enabled(bool en);` / `bool detect_enabled(void);`
  - `detect_result_t detect_observe(uint32_t hash, int8_t rssi, uint16_t vendor, uint16_t epoch);`
  - `void detect_on_epoch_change(uint16_t epoch);`
  - `size_t detect_threat_count(void);`
  - `typedef struct { uint32_t hash; uint16_t vendor; uint8_t epochs; int8_t best_rssi; uint16_t first_epoch; uint16_t last_epoch; } detect_threat_t;`
  - `bool detect_threat_at(size_t i, detect_threat_t *out);`
  - Tunables `DETECT_EPOCH_STRIKES=3`, `DETECT_MIN_SIGHTINGS=2`, `DETECT_MAX_CANDIDATES=48`, `DETECT_MAX_THREATS=8`.

- [ ] **Step 1: Write `main/detect.h` (interface for this task)**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- tunables (spec §3/§4) ---
#ifndef DETECT_EPOCH_STRIKES
#define DETECT_EPOCH_STRIKES     3      // distinct credited epochs -> confirmed follower
#endif
#ifndef DETECT_MIN_SIGHTINGS
#define DETECT_MIN_SIGHTINGS     2      // adv reports in an epoch to earn a presence-credit
#endif
#ifndef DETECT_AGE_OUT_EPOCHS
#define DETECT_AGE_OUT_EPOCHS    8      // drop an un-promoted candidate unseen this many epochs
#endif
#ifndef DETECT_MAX_CANDIDATES
#define DETECT_MAX_CANDIDATES    48     // RAM candidate table (LRU-evicted)
#endif
#ifndef DETECT_MAX_THREATS
#define DETECT_MAX_THREATS       8      // confirmed threats retained + persisted
#endif
#ifndef DETECT_LOCATE_RSSI_DELTA
#define DETECT_LOCATE_RSSI_DELTA 6      // dB change that emits a locate update
#endif
#ifndef DETECT_LOCATE_MIN_MS
#define DETECT_LOCATE_MIN_MS     10000  // min interval between locate updates
#endif

typedef enum { DETECT_NONE = 0, DETECT_CONFIRM, DETECT_KNOWN } detect_result_t;

// A confirmed follower (also the persisted record shape). Hash-only; no raw MAC.
typedef struct {
    uint32_t hash;
    uint16_t vendor;
    uint8_t  epochs;        // distinct credited epochs at/after confirmation (>= STRIKES)
    int8_t   best_rssi;
    uint16_t first_epoch;
    uint16_t last_epoch;
} detect_threat_t;

// --- pure core (no radio, no NVS in the decision path) ---
void            detect_reset(void);                 // clear all RAM state (tests + boot)
void            detect_set_enabled(bool en);
bool            detect_enabled(void);
detect_result_t detect_observe(uint32_t hash, int8_t rssi, uint16_t vendor, uint16_t epoch);
void            detect_on_epoch_change(uint16_t epoch);

size_t          detect_threat_count(void);
bool            detect_threat_at(size_t i, detect_threat_t *out);
```

- [ ] **Step 2: Add the three failing tests to `main/churn_selftest.c`**

Add `#include "detect.h"` near the other includes (after line 12), and these functions before `churn_selftest_run`:

```c
static void test_detect_epochs(void)
{
    detect_reset();
    // Same device (hash 0xAAAA), 2 sightings each across 3 distinct epochs -> CONFIRM on the 3rd.
    detect_result_t r = DETECT_NONE;
    for (uint16_t e = 1; e <= 3; e++) {
        detect_on_epoch_change(e);
        r = detect_observe(0xAAAA, -50, 0x0075, e);   // sighting 1: credit pending
        detect_result_t r2 = detect_observe(0xAAAA, -50, 0x0075, e); // sighting 2: credit
        if (e < 3) ST_CHECK(r == DETECT_NONE && r2 == DETECT_NONE, "no confirm before 3 epochs");
        else       ST_CHECK(r2 == DETECT_CONFIRM, "confirm on the 3rd distinct epoch");
    }
    ST_CHECK(detect_threat_count() == 1, "one confirmed threat recorded");
    detect_threat_t t;
    ST_CHECK(detect_threat_at(0, &t) && t.hash == 0xAAAA && t.epochs >= DETECT_EPOCH_STRIKES,
             "threat record carries the hash + >=3 epochs");
    // A subsequent sighting of a confirmed threat returns KNOWN, not CONFIRM.
    ST_CHECK(detect_observe(0xAAAA, -40, 0x0075, 4) == DETECT_KNOWN, "confirmed threat -> KNOWN");
}

static void test_detect_presence(void)
{
    detect_reset();
    // A single sighting per epoch never earns a credit -> never confirms.
    for (uint16_t e = 1; e <= 6; e++) {
        detect_on_epoch_change(e);
        ST_CHECK(detect_observe(0xBBBB, -50, 0, e) == DETECT_NONE, "single-sighting epoch earns no credit");
    }
    ST_CHECK(detect_threat_count() == 0, "no confirm without meaningful presence");
}

static void test_detect_no_false_confirm(void)
{
    detect_reset();
    // Many one-shot devices, one sighting each in one epoch: a drive-by pattern, never confirms.
    detect_on_epoch_change(1);
    for (uint32_t h = 0; h < 20; h++) ST_CHECK(detect_observe(0x1000 + h, -60, 0, 1) == DETECT_NONE,
                                               "drive-by device does not confirm");
    // Two credited sightings but only in ONE epoch: still below the strike bar.
    detect_observe(0x2222, -50, 0, 1); detect_observe(0x2222, -50, 0, 1);
    ST_CHECK(detect_threat_count() == 0, "presence in a single epoch is not a follower");
}
```

Add the calls inside `churn_selftest_run()`, just before the final `ESP_LOGW(... "SELFTEST: ...")`:

```c
    test_detect_epochs();
    test_detect_presence();
    test_detect_no_false_confirm();
```

- [ ] **Step 3: Add `detect.c` to the build and run the self-test to verify it fails to build/link**

Edit `main/CMakeLists.txt:2` — append `"detect.c"` to the SRCS list. Set `#define CHURN_SELFTEST 1` in `main/simulacra_main.c`, then build (BUILD/FLASH/READ-C6 recipe).
Expected: **build fails** — `undefined reference to detect_reset / detect_observe / ...` (header exists, no implementation yet).

- [ ] **Step 4: Write `main/detect.c` (minimal implementation for this task)**

```c
#include <string.h>
#include "detect.h"

typedef struct {
    bool     used;
    uint32_t hash;
    uint16_t vendor;
    uint16_t cur_epoch;            // epoch of the current run of sightings
    uint8_t  sightings_this_epoch;
    bool     credited;             // already credited cur_epoch?
    uint8_t  distinct_epochs;      // credited distinct epochs so far
    int8_t   best_rssi;
    uint16_t first_epoch;
    uint16_t last_seen_epoch;
    uint32_t lru;                  // last-use tick (for LRU eviction, Task 2)
} candidate_t;

static candidate_t    s_cand[DETECT_MAX_CANDIDATES];
static detect_threat_t s_threat[DETECT_MAX_THREATS];
static size_t         s_threat_n;
static uint32_t       s_lru;
static bool           s_enabled = true;

void detect_reset(void)
{
    memset(s_cand, 0, sizeof(s_cand));
    memset(s_threat, 0, sizeof(s_threat));
    s_threat_n = 0;
    s_lru = 0;
    s_enabled = true;
}

void detect_set_enabled(bool en) { s_enabled = en; }
bool detect_enabled(void)        { return s_enabled; }

static detect_threat_t *threat_find(uint32_t hash)
{
    for (size_t i = 0; i < s_threat_n; i++) if (s_threat[i].hash == hash) return &s_threat[i];
    return NULL;
}

// Find an existing candidate, else allocate a fresh one (LRU-evict when full — Task 2 hardens
// the eviction; here we take the first free slot or the lowest-lru slot).
static candidate_t *cand_find_or_alloc(uint32_t hash, uint16_t epoch, int8_t rssi)
{
    candidate_t *victim = NULL;
    for (size_t i = 0; i < DETECT_MAX_CANDIDATES; i++) {
        if (s_cand[i].used && s_cand[i].hash == hash) return &s_cand[i];
        if (!s_cand[i].used) { if (!victim) victim = &s_cand[i]; }
        else if (!victim || s_cand[i].lru < victim->lru) { /* track lru victim only if no free */ }
    }
    if (!victim) {   // no free slot: evict lowest-lru used slot
        victim = &s_cand[0];
        for (size_t i = 1; i < DETECT_MAX_CANDIDATES; i++)
            if (s_cand[i].lru < victim->lru) victim = &s_cand[i];
    }
    memset(victim, 0, sizeof(*victim));
    victim->used = true; victim->hash = hash;
    victim->cur_epoch = epoch; victim->first_epoch = epoch;
    victim->best_rssi = rssi;
    return victim;
}

static detect_threat_t *promote(candidate_t *c)
{
    detect_threat_t *t = threat_find(c->hash);
    if (!t) {
        if (s_threat_n < DETECT_MAX_THREATS) t = &s_threat[s_threat_n++];
        else {   // full: overwrite the least-recently-seen threat
            t = &s_threat[0];
            for (size_t i = 1; i < s_threat_n; i++)
                if (s_threat[i].last_epoch < t->last_epoch) t = &s_threat[i];
        }
    }
    t->hash = c->hash; t->vendor = c->vendor; t->epochs = c->distinct_epochs;
    t->best_rssi = c->best_rssi; t->first_epoch = c->first_epoch; t->last_epoch = c->cur_epoch;
    c->used = false;   // candidate graduates to a threat
    return t;
}

detect_result_t detect_observe(uint32_t hash, int8_t rssi, uint16_t vendor, uint16_t epoch)
{
    if (!s_enabled) return DETECT_NONE;

    detect_threat_t *t = threat_find(hash);
    if (t) {
        if (rssi > t->best_rssi) t->best_rssi = rssi;
        t->last_epoch = epoch;
        return DETECT_KNOWN;
    }

    candidate_t *c = cand_find_or_alloc(hash, epoch, rssi);
    c->vendor = vendor;
    c->last_seen_epoch = epoch;
    c->lru = ++s_lru;
    if (rssi > c->best_rssi) c->best_rssi = rssi;

    if (epoch != c->cur_epoch) {          // rolled into a new epoch
        c->cur_epoch = epoch;
        c->sightings_this_epoch = 0;
        c->credited = false;
    }
    if (c->sightings_this_epoch < 255) c->sightings_this_epoch++;

    if (!c->credited && c->sightings_this_epoch >= DETECT_MIN_SIGHTINGS) {
        c->credited = true;
        c->distinct_epochs++;
        if (c->distinct_epochs >= DETECT_EPOCH_STRIKES) { promote(c); return DETECT_CONFIRM; }
    }
    return DETECT_NONE;
}

void detect_on_epoch_change(uint16_t epoch) { (void)epoch; /* aging added in Task 2 */ }

size_t detect_threat_count(void) { return s_threat_n; }

bool detect_threat_at(size_t i, detect_threat_t *out)
{
    if (i >= s_threat_n || !out) return false;
    *out = s_threat[i];
    return true;
}
```

- [ ] **Step 5: Run the self-test to verify it passes**

Build+flash+read (BUILD/FLASH/READ-C6). Expected serial: `SELFTEST: PASS (N/N)` with the three new test groups included and `fails=0`.

- [ ] **Step 6: Revert the self-test toggle and commit**

Set `#define CHURN_SELFTEST 0` back in `main/simulacra_main.c` (production default). Commit:

```bash
git add main/detect.h main/detect.c main/CMakeLists.txt main/churn_selftest.c
git commit -F- <<'EOF'
feat(m9): detection core — location-epochs + 3-strike confirmation

Pure detect.c: candidate table, per-epoch presence-credit (MIN_SIGHTINGS=2),
confirm at DETECT_EPOCH_STRIKES=3 distinct credited epochs. Hash-only, no radio
or NVS in the decision path. Self-tests: epochs, presence floor, no-false-confirm.
EOF
```

---

## Task 2: Candidate aging + LRU bound

**Files:**
- Modify: `main/detect.c` (`detect_on_epoch_change`, verify LRU eviction)
- Modify: `main/churn_selftest.c` (add `test_detect_ageout` + call)

**Interfaces:**
- Consumes: Task 1's `detect_observe`, `detect_on_epoch_change`, `detect_reset`, `DETECT_MAX_CANDIDATES`, `DETECT_AGE_OUT_EPOCHS`.
- Produces: no new public signatures; `detect_on_epoch_change` now ages out and the table stays bounded.

- [ ] **Step 1: Write the failing test in `main/churn_selftest.c`**

```c
static void test_detect_ageout(void)
{
    detect_reset();
    // Device seen once at epoch 1, then absent. After AGE_OUT_EPOCHS it must be forgotten,
    // so a later re-appearance starts fresh (never accumulates a stale credit).
    detect_on_epoch_change(1);
    detect_observe(0xCAFE, -50, 0, 1); detect_observe(0xCAFE, -50, 0, 1);   // credited epoch 1
    for (uint16_t e = 2; e <= 1 + DETECT_AGE_OUT_EPOCHS; e++) detect_on_epoch_change(e);
    // Now re-appear: because it was aged out, it must take a full fresh run to confirm.
    uint16_t e0 = 2 + DETECT_AGE_OUT_EPOCHS;
    detect_result_t r = DETECT_NONE;
    for (uint16_t e = e0; e < e0 + 2; e++) { detect_on_epoch_change(e);
        detect_observe(0xCAFE, -50, 0, e); r = detect_observe(0xCAFE, -50, 0, e); }
    ST_CHECK(r == DETECT_NONE, "aged-out candidate does not carry its old epoch credit");

    // LRU bound: pushing many distinct devices never overflows the table.
    detect_reset();
    for (uint32_t h = 0; h < DETECT_MAX_CANDIDATES * 3u; h++) detect_observe(0x8000 + h, -60, 0, 1);
    ST_CHECK(detect_threat_count() == 0, "LRU churn of distinct one-shot devices confirms nothing");
}
```
Add `test_detect_ageout();` to `churn_selftest_run()` after `test_detect_no_false_confirm();`.

- [ ] **Step 2: Run the self-test to verify it fails**

Set `CHURN_SELFTEST 1`, build+flash+read. Expected: `SELFTEST: FAIL` with `FAIL: aged-out candidate does not carry its old epoch credit` (aging is a no-op from Task 1).

- [ ] **Step 3: Implement aging in `main/detect.c`**

Replace the stub `detect_on_epoch_change`:

```c
void detect_on_epoch_change(uint16_t epoch)
{
    for (size_t i = 0; i < DETECT_MAX_CANDIDATES; i++) {
        if (!s_cand[i].used) continue;
        if (s_cand[i].distinct_epochs >= DETECT_EPOCH_STRIKES) continue;  // (already promoted anyway)
        uint16_t gap = (uint16_t)(epoch - s_cand[i].last_seen_epoch);
        if (gap >= DETECT_AGE_OUT_EPOCHS) s_cand[i].used = false;         // forget the follower-less device
    }
}
```

- [ ] **Step 4: Run the self-test to verify it passes**

Build+flash+read. Expected: `SELFTEST: PASS (N/N)`.

- [ ] **Step 5: Revert toggle and commit**

`CHURN_SELFTEST 0`, then:
```bash
git add main/detect.c main/churn_selftest.c
git commit -m "feat(m9): age out follower-less candidates (RAM bound + no stale credit)"
```

---

## Task 3: Locate-throttle helper

**Files:**
- Modify: `main/detect.h` (declare `detect_locate_due`)
- Modify: `main/detect.c` (implement it)
- Modify: `main/churn_selftest.c` (add `test_detect_locate_throttle` + call)

**Interfaces:**
- Produces: `bool detect_locate_due(int8_t rssi, int8_t last_rssi, uint32_t now_ms, uint32_t last_ms);` — pure; true when a `THREAT locate` line should be emitted (RSSI moved ≥ `DETECT_LOCATE_RSSI_DELTA` **or** ≥ `DETECT_LOCATE_MIN_MS` elapsed). The coordinator (Task 7) owns the per-threat `last_rssi`/`last_ms` state and calls this.

- [ ] **Step 1: Declare in `main/detect.h`** (add under the pure-core section):

```c
// Locate-throttle (pure): should a `THREAT locate` line be emitted for this sighting?
bool detect_locate_due(int8_t rssi, int8_t last_rssi, uint32_t now_ms, uint32_t last_ms);
```

- [ ] **Step 2: Write the failing test in `main/churn_selftest.c`**

```c
static void test_detect_locate_throttle(void)
{
    // Elapsed >= MIN_MS -> due regardless of RSSI.
    ST_CHECK(detect_locate_due(-50, -50, DETECT_LOCATE_MIN_MS, 0), "locate due after min interval");
    // Small RSSI move within the interval -> not due.
    ST_CHECK(!detect_locate_due(-50, -47, 1000, 0), "small RSSI move within interval is throttled");
    // Big RSSI move within the interval -> due (getting-warmer).
    ST_CHECK(detect_locate_due(-40, -50, 1000, 0), "RSSI delta >= threshold emits immediately");
    ST_CHECK(detect_locate_due(-60, -50, 1000, 0), "RSSI delta is symmetric (getting colder counts)");
}
```
Add `test_detect_locate_throttle();` to `churn_selftest_run()`.

- [ ] **Step 3: Run the self-test to verify it fails** (link error: `detect_locate_due` undefined). `CHURN_SELFTEST 1`, build.

- [ ] **Step 4: Implement in `main/detect.c`**

```c
bool detect_locate_due(int8_t rssi, int8_t last_rssi, uint32_t now_ms, uint32_t last_ms)
{
    if ((uint32_t)(now_ms - last_ms) >= DETECT_LOCATE_MIN_MS) return true;
    int d = (int)rssi - (int)last_rssi;
    if (d < 0) d = -d;
    return d >= DETECT_LOCATE_RSSI_DELTA;
}
```

- [ ] **Step 5: Run the self-test to verify it passes.** Build+flash+read → `PASS`.

- [ ] **Step 6: Revert toggle and commit**

```bash
git add main/detect.h main/detect.c main/churn_selftest.c
git commit -m "feat(m9): pure locate-throttle helper (RSSI-delta OR min-interval)"
```

---

## Task 4: Self-exclusion helper

**Files:**
- Modify: `main/detect.h` (declare `detect_mac_in_set`)
- Modify: `main/detect.c` (implement it)
- Modify: `main/churn_selftest.c` (add `test_detect_self_exclude` + call)

**Interfaces:**
- Produces: `bool detect_mac_in_set(const uint8_t mac[6], const uint8_t set[][6], size_t n);` — pure; the coordinator (Task 7) builds the set from `churn_active_at()` and skips any report whose MAC is in it, so the decoy never flags itself.

- [ ] **Step 1: Declare in `main/detect.h`:**

```c
// Self-exclusion (pure): is `mac` present in the given set of `n` 6-byte MACs?
bool detect_mac_in_set(const uint8_t mac[6], const uint8_t set[][6], size_t n);
```

- [ ] **Step 2: Write the failing test in `main/churn_selftest.c`**

```c
static void test_detect_self_exclude(void)
{
    const uint8_t set[3][6] = {
        {0x01,0x02,0x03,0x04,0x05,0xC0},
        {0x11,0x12,0x13,0x14,0x15,0xC1},
        {0x21,0x22,0x23,0x24,0x25,0xC2},
    };
    const uint8_t self_mac[6] = {0x11,0x12,0x13,0x14,0x15,0xC1};
    const uint8_t other[6]    = {0xAA,0xBB,0xCC,0xDD,0xEE,0xC3};
    ST_CHECK(detect_mac_in_set(self_mac, set, 3), "our own active MAC is recognized (excluded)");
    ST_CHECK(!detect_mac_in_set(other, set, 3),   "a foreign MAC is not excluded");
    ST_CHECK(!detect_mac_in_set(self_mac, set, 0), "empty set excludes nothing");
}
```
Add `test_detect_self_exclude();` to `churn_selftest_run()`.

- [ ] **Step 3: Run the self-test to verify it fails** (undefined `detect_mac_in_set`).

- [ ] **Step 4: Implement in `main/detect.c`**

```c
bool detect_mac_in_set(const uint8_t mac[6], const uint8_t set[][6], size_t n)
{
    for (size_t i = 0; i < n; i++) if (memcmp(mac, set[i], 6) == 0) return true;
    return false;
}
```

- [ ] **Step 5: Run the self-test to verify it passes.** → `PASS`.

- [ ] **Step 6: Revert toggle and commit**

```bash
git add main/detect.h main/detect.c main/churn_selftest.c
git commit -m "feat(m9): pure self-exclusion helper (skip our own decoy MACs)"
```

---

## Task 5: Per-install salt + confirmed-threat NVS persistence

**Files:**
- Modify: `main/detect.h` (declare salt + persist functions + `detect_drain_pending`)
- Modify: `main/detect.c` (NVS section + pending flag)
- Modify: `main/churn_selftest.c` (add `test_detect_nvs` + call)

**Interfaces:**
- Produces:
  - `uint32_t detect_load_salt(void);` — load-or-create the persistent per-install salt (NVS namespace `"splinter"`, key `"detect_salt"`).
  - `int detect_save_nvs(void);` / `int detect_load_nvs(void);` — persist/restore confirmed threats (key `"detect_thr"`), return `0` on success. Candidates are never persisted.
  - `bool detect_drain_pending(detect_threat_t *out);` — true + fills `*out` once per newly-confirmed threat; clears the flag. Drained by `coexist_task` (Task 7) to trigger the deferred NVS persist + LED.

- [ ] **Step 1: Declare in `main/detect.h`** (add a persistence section):

```c
// --- persistence (NVS namespace "splinter"; called by the coordinator, not the decision path) ---
uint32_t detect_load_salt(void);        // load-or-create the per-install detection salt
int      detect_save_nvs(void);         // persist confirmed threats; 0 = ok
int      detect_load_nvs(void);         // restore confirmed threats; 0 = ok
// Drain the newly-confirmed-threat flag (coexist_task): true + *out once per confirmation.
bool     detect_drain_pending(detect_threat_t *out);
```

- [ ] **Step 2: Write the failing test in `main/churn_selftest.c`**

```c
static void test_detect_nvs(void)
{
    detect_reset();
    // Confirm a threat, then round-trip the threat table through NVS.
    for (uint16_t e = 1; e <= 3; e++) { detect_on_epoch_change(e);
        detect_observe(0xD00D, -44, 0x004C, e); detect_observe(0xD00D, -44, 0x004C, e); }
    ST_CHECK(detect_threat_count() == 1, "threat confirmed before persist");

    // Pending-confirm drains exactly once.
    detect_threat_t p;
    ST_CHECK(detect_drain_pending(&p) && p.hash == 0xD00D, "pending confirm drains the new threat");
    ST_CHECK(!detect_drain_pending(&p), "pending flag clears after draining");

    ST_CHECK(detect_save_nvs() == 0, "threats save to NVS");
    detect_reset();
    ST_CHECK(detect_threat_count() == 0, "reset clears RAM threats");
    ST_CHECK(detect_load_nvs() == 0, "threats load from NVS");
    ST_CHECK(detect_threat_count() == 1, "one threat restored");
    detect_threat_t t;
    ST_CHECK(detect_threat_at(0, &t) && t.hash == 0xD00D && t.vendor == 0x004C && t.epochs >= 3,
             "restored threat round-trips hash/vendor/epochs");

    // Salt persists (stable across a load), so cross-boot hashes are stable by design.
    uint32_t s1 = detect_load_salt();
    uint32_t s2 = detect_load_salt();
    ST_CHECK(s1 == s2 && s1 != 0, "per-install salt is stable and non-zero");
}
```
Add `test_detect_nvs();` to `churn_selftest_run()`.

- [ ] **Step 3: Run the self-test to verify it fails** (undefined salt/persist/drain functions).

- [ ] **Step 4: Implement the NVS section + pending flag in `main/detect.c`**

Add near the top includes:
```c
#include "nvs.h"
#include "esp_random.h"
```
Add a pending-threat static beside the other statics:
```c
static bool           s_pending;
static detect_threat_t s_pending_threat;
```
In `promote()`, after filling `*t`, record the pending confirmation:
```c
    s_pending = true; s_pending_threat = *t;
```
In `detect_reset()`, also clear it: add `s_pending = false;`.
Then append the persistence functions:
```c
#define DETECT_NVS_NS    "splinter"       // reuse the existing namespace (do not rename)
#define DETECT_KEY_SALT  "detect_salt"
#define DETECT_KEY_THR   "detect_thr"
#define DETECT_THR_MAGIC 0x4D394454u       // 'M9DT'

bool detect_drain_pending(detect_threat_t *out)
{
    if (!s_pending) return false;
    if (out) *out = s_pending_threat;
    s_pending = false;
    return true;
}

uint32_t detect_load_salt(void)
{
    nvs_handle_t h; uint32_t salt = 0;
    if (nvs_open(DETECT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return esp_random() | 1u;
    if (nvs_get_u32(h, DETECT_KEY_SALT, &salt) != ESP_OK || salt == 0) {
        salt = esp_random() | 1u;                 // never zero (test asserts non-zero)
        nvs_set_u32(h, DETECT_KEY_SALT, salt);
        nvs_commit(h);
    }
    nvs_close(h);
    return salt;
}

typedef struct { uint32_t magic; uint16_t count; detect_threat_t thr[DETECT_MAX_THREATS]; } detect_blob_t;

int detect_save_nvs(void)
{
    detect_blob_t b; memset(&b, 0, sizeof(b));
    b.magic = DETECT_THR_MAGIC; b.count = (uint16_t)s_threat_n;
    for (size_t i = 0; i < s_threat_n; i++) b.thr[i] = s_threat[i];
    nvs_handle_t h;
    if (nvs_open(DETECT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, DETECT_KEY_THR, &b, sizeof(b));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (int)e;
}

int detect_load_nvs(void)
{
    detect_blob_t b; size_t len = sizeof(b);
    nvs_handle_t h;
    if (nvs_open(DETECT_NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_get_blob(h, DETECT_KEY_THR, &b, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(b) || b.magic != DETECT_THR_MAGIC) return -1;
    s_threat_n = (b.count <= DETECT_MAX_THREATS) ? b.count : DETECT_MAX_THREATS;
    for (size_t i = 0; i < s_threat_n; i++) s_threat[i] = b.thr[i];
    return 0;
}
```

- [ ] **Step 5: Run the self-test to verify it passes.** Build+flash+read → `PASS`. (If a stale NVS blob from a prior run interferes, erase once: `idf.py -p <PORT> erase-flash` then reflash.)

- [ ] **Step 6: Revert toggle and commit**

```bash
git add main/detect.h main/detect.c main/churn_selftest.c
git commit -F- <<'EOF'
feat(m9): per-install salt + confirmed-threat NVS persistence

Persist confirmed threats only (blob key detect_thr) + a stable per-install
salt (detect_salt) in the existing "splinter" namespace. Pending-confirm flag
drains once per confirmation for the coordinator to persist off the BLE callback.
EOF
```

---

## Task 6: Observe report callback

**Files:**
- Modify: `main/observe.h` (callback typedef + setter)
- Modify: `main/observe.c` (fire the callback in `observe_gap_event` before the MAC is hashed)

**Interfaces:**
- Produces:
  - `typedef void (*observe_report_cb_t)(const uint8_t mac[6], int8_t rssi, uint16_t company_id);`
  - `void observe_set_report_cb(observe_report_cb_t cb);` — `NULL` disables. Fired for every EXT_DISC report, before `observe_ingest` drops the MAC. Observe gains no dependency on `detect`.
- Verification: build-only (the callback fires inside a NimBLE host callback that the radio-idle self-test cannot exercise; behavior is proven in Task 10 hardware acceptance).

- [ ] **Step 1: Declare in `main/observe.h`** (add after the live-radio section):

```c
// --- report tap (M9): observe publishes each raw report before hashing it away ---
typedef void (*observe_report_cb_t)(const uint8_t mac[6], int8_t rssi, uint16_t company_id);
// Register a callback fired for every scan report (mac, rssi, company) BEFORE the MAC is
// hashed/dropped. NULL disables. Observe stays decoupled from any consumer.
void observe_set_report_cb(observe_report_cb_t cb);
```

- [ ] **Step 2: Implement in `main/observe.c`**

Add a static near the other file-scope statics (e.g. beside `s_window_mode`):
```c
static observe_report_cb_t s_report_cb;
```
Add the setter (near `observe_start`):
```c
void observe_set_report_cb(observe_report_cb_t cb) { s_report_cb = cb; }
```
In `observe_gap_event`, immediately after `company` is parsed and before `observe_ingest` (currently line 138), fire the callback:
```c
    if (s_report_cb) s_report_cb(d->addr.val, d->rssi, company);   // raw MAC still live here
    observe_ingest(&s_model, d->addr.val, now, company, d->rssi, d->legacy_event_type);
```

- [ ] **Step 3: Build (production default) to verify it compiles**

Ensure `CHURN_SELFTEST 0`. Build (BUILD/FLASH/READ-C6, build step only). Expected: `Project build complete`. No behavior change yet (no callback registered).

- [ ] **Step 4: Commit**

```bash
git add main/observe.h main/observe.c
git commit -m "feat(m9): observe publishes raw reports via a registered callback (pre-hash)"
```

---

## Task 7: Coordinator wiring — epochs, adapter, alerting, deferred persist

**Files:**
- Modify: `main/coexist.h` (declare `coexist_current_epoch`)
- Modify: `main/coexist.c` (epoch counter + bump in `coexist_reprofile`; the wiring adapter callback; drain in `coexist_task`)

**Interfaces:**
- Consumes: `detect_observe`, `detect_on_epoch_change`, `detect_mac_in_set`, `detect_locate_due`, `detect_drain_pending`, `detect_save_nvs`, `detect_load_salt` (from `detect.h`); `observe_set_report_cb` (Task 6); `observe_model`, `drift_score` (existing); `churn_active_at`, `churn_active_count`, `identity.addr` (existing); `observe_hash_mac` is per-boot — M9 uses its **own** salted hash (below), not `observe_hash_mac`.
- Produces: `uint16_t coexist_current_epoch(void);` (the adapter reads it when a report fires).
- Verification: build-only here; end-to-end behavior proven in Task 10.

- [ ] **Step 1: Declare the epoch accessor in `main/coexist.h`:**

```c
// Current location-epoch (M9): advanced when a re-profile measures a materially changed room.
uint16_t coexist_current_epoch(void);
```

- [ ] **Step 2: Add the epoch counter, salt, and locate-state statics to `main/coexist.c`**

Near the top of `coexist.c` (after includes; add `#include "detect.h"` and `#include "esp_log.h"` if not present):
```c
#define DETECT_EPOCH_DRIFT 0.45f          // detection-owned; separate from anti-entourage thresh

static uint16_t s_epoch;                  // location-epoch counter (M9)
static uint32_t s_detect_salt;            // per-install salt (M9)

// Small per-threat locate-throttle state (hash -> last rssi/time), coordinator-owned.
static struct { uint32_t hash; int8_t last_rssi; uint32_t last_ms; bool used; }
       s_locate[DETECT_MAX_THREATS];

uint16_t coexist_current_epoch(void) { return s_epoch; }
```

- [ ] **Step 3: Add the wiring-adapter callback to `main/coexist.c`**

```c
// Build the self-exclusion set from the churn active identities.
static size_t coexist_self_macs(uint8_t out[][6], size_t max)
{
    size_t n = 0;
    for (size_t s = 0; s < churn_active_count() && n < max; s++) {
        const identity_t *id = churn_active_at(s);
        if (id) memcpy(out[n++], id->addr, 6);
    }
    return n;
}

// M9 per-install-salted FNV-1a over the MAC (stable across sweeps/reboots — deliberate).
static uint32_t coexist_detect_hash(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_detect_salt;
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

static void coexist_locate_emit(uint32_t hash, uint32_t id_prefix, int8_t rssi, uint32_t now_ms)
{
    // find-or-alloc a locate slot for this confirmed threat
    int slot = -1;
    for (size_t i = 0; i < DETECT_MAX_THREATS; i++) {
        if (s_locate[i].used && s_locate[i].hash == hash) { slot = (int)i; break; }
        if (slot < 0 && !s_locate[i].used) slot = (int)i;
    }
    if (slot < 0) return;
    if (!s_locate[slot].used) {                      // first sighting since confirm -> emit + seed
        s_locate[slot] = (typeof(s_locate[slot])){ .hash = hash, .last_rssi = rssi,
                                                    .last_ms = now_ms, .used = true };
        ESP_LOGW(TAG, "THREAT locate id=%04x rssi=%d seen=+0s", (unsigned)id_prefix, rssi);
        return;
    }
    if (detect_locate_due(rssi, s_locate[slot].last_rssi, now_ms, s_locate[slot].last_ms)) {
        ESP_LOGW(TAG, "THREAT locate id=%04x rssi=%d seen=+%us", (unsigned)id_prefix, rssi,
                 (unsigned)((now_ms - s_locate[slot].last_ms) / 1000));
        s_locate[slot].last_rssi = rssi; s_locate[slot].last_ms = now_ms;
    }
}

// Registered on observe: fires for every raw report (NimBLE host-task context).
static void coexist_on_report(const uint8_t mac[6], int8_t rssi, uint16_t company)
{
    if (!detect_enabled()) return;
    uint8_t self[8][6]; size_t nself = coexist_self_macs(self, 8);
    if (detect_mac_in_set(mac, self, nself)) return;         // never flag our own decoys

    uint32_t hash = coexist_detect_hash(mac);
    uint16_t epoch = s_epoch;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    detect_result_t r = detect_observe(hash, rssi, company, epoch);
    if (r == DETECT_CONFIRM) {
        ESP_LOGW(TAG, "THREAT confirmed id=%04x vendor=0x%04x epochs=%u rssi=%d "
                      "mac=%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)(hash & 0xFFFF), company, DETECT_EPOCH_STRIKES, rssi,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else if (r == DETECT_KNOWN) {
        coexist_locate_emit(hash, hash & 0xFFFF, rssi, now);
    }
}
```

- [ ] **Step 4: Bump the epoch in `coexist_reprofile`**

In `main/coexist.c`, in `coexist_reprofile`, immediately after `float score = drift_score(&prev, cur);` (currently line 102), add:
```c
    if (prev.sweeps > 0 && score > DETECT_EPOCH_DRIFT) {   // materially new room -> new location-epoch
        s_epoch++;
        detect_on_epoch_change(s_epoch);
        ESP_LOGW(TAG, "epoch -> %u (drift=%.3f)", (unsigned)s_epoch, score);
    }
```

- [ ] **Step 5: Drain pending confirmations in `coexist_task`**

In `main/coexist.c`, in `coexist_task`'s loop, right after `coexist_decay_accel();`, add:
```c
        detect_threat_t nt;
        if (detect_drain_pending(&nt)) {
            int rc = detect_save_nvs();                     // persist off the BLE callback
            ESP_LOGW(TAG, "THREAT persisted id=%04x rc=%d", (unsigned)(nt.hash & 0xFFFF), rc);
        }
```

- [ ] **Step 6: Initialize the adapter in `coexist_start`**

In `main/coexist.c`, in `coexist_start()` (before spawning `coexist_task`), wire the detector to observe and load the salt/threats:
```c
    s_detect_salt = detect_load_salt();
    detect_load_nvs();                 // restore any previously-confirmed threats (best-effort)
    observe_set_report_cb(coexist_on_report);
```

- [ ] **Step 7: Build (production default) to verify it compiles**

`CHURN_SELFTEST 0`. Build (BUILD/FLASH/READ-C6, build step). Expected: `Project build complete`.

- [ ] **Step 8: Commit**

```bash
git add main/coexist.h main/coexist.c
git commit -F- <<'EOF'
feat(m9): coordinator wiring — location-epochs, detector adapter, alerting

coexist owns the epoch counter (bumped when reprofile drift > 0.45, both
personas), the salted self-excluding report adapter feeding detect_observe,
THREAT confirmed/locate serial lines, and the deferred NVS persist drained off
the BLE callback in coexist_task.
EOF
```

---

## Task 8: Default-path init + master enable

**Files:**
- Modify: `main/simulacra_main.c` (honor `SIMULACRA_DETECT`; reset detector state at boot)

**Interfaces:**
- Consumes: `detect_set_enabled`, `detect_reset` (from `detect.h`). Salt load + threat restore + callback registration already happen in `coexist_start` (Task 7); this task adds the compile-time master switch and a clean boot state.

- [ ] **Step 1: Add the `SIMULACRA_DETECT` default near the other build-mode defaults in `main/simulacra_main.c`** (beside the `SIMULACRA_OBSERVE` block, ~line 66):

```c
// Threat Radar (M9): passive follower detection alongside the decoy. Default ON.
#ifndef SIMULACRA_DETECT
#define SIMULACRA_DETECT 1
#endif
```

- [ ] **Step 2: Gate detection in the default decoy path**

Add `#include "detect.h"` with the other module includes. In the default path (`#else` branch, before `coexist_start();` at line 115):
```c
    detect_reset();
    detect_set_enabled(SIMULACRA_DETECT);   // compile-time master enable (default on)
```
(`coexist_start` then loads the salt + persisted threats and registers the observe callback; when disabled, `coexist_on_report` early-returns via `detect_enabled()`.)

- [ ] **Step 3: Build both personas to verify it compiles**

`CHURN_SELFTEST 0`. Build C6 (`Project build complete`). Then retarget and build C5 (BUILD/FLASH/READ-C5 build step) to confirm both persona arms compile.

- [ ] **Step 4: Commit**

```bash
git add main/simulacra_main.c
git commit -m "feat(m9): wire Threat Radar into the default decoy (SIMULACRA_DETECT=1)"
```

---

## Task 9: Optional status LED (board-gated)

**Files:**
- Modify: `main/coexist.c` (blink an LED while a confirmed threat is active, behind `SIMULACRA_DETECT_LED_GPIO`)

**Interfaces:**
- Consumes: `detect_threat_count()`; ESP-IDF `driver/gpio.h`. Compiled only when `SIMULACRA_DETECT_LED_GPIO` is defined (unset by default → no LED code, zero cost). This is glue; verified by build with the macro set.

- [ ] **Step 1: Add the LED hook in `main/coexist.c`**

At file scope:
```c
#ifdef SIMULACRA_DETECT_LED_GPIO
#include "driver/gpio.h"
static void coexist_detect_led_init(void)
{
    gpio_reset_pin((gpio_num_t)SIMULACRA_DETECT_LED_GPIO);
    gpio_set_direction((gpio_num_t)SIMULACRA_DETECT_LED_GPIO, GPIO_MODE_OUTPUT);
}
static void coexist_detect_led_tick(uint32_t now_ms)
{
    static uint32_t last; static int on;
    bool active = detect_threat_count() > 0;
    if (!active) { gpio_set_level((gpio_num_t)SIMULACRA_DETECT_LED_GPIO, 0); return; }
    if (now_ms - last >= 500) { on = !on; last = now_ms;                // slow blink
        gpio_set_level((gpio_num_t)SIMULACRA_DETECT_LED_GPIO, on); }
}
#else
static inline void coexist_detect_led_init(void) {}
static inline void coexist_detect_led_tick(uint32_t now_ms) { (void)now_ms; }
#endif
```
Call `coexist_detect_led_init();` in `coexist_start()` (after the detector wiring) and `coexist_detect_led_tick(now);` in `coexist_task`'s loop (after the drain block, using the loop's `now`).

- [ ] **Step 2: Build twice to verify both arms compile**

`CHURN_SELFTEST 0`. Build default C6 (LED off, `Project build complete`). Then prove the LED arm compiles by **temporarily** adding `#define SIMULACRA_DETECT_LED_GPIO 15` at the top of `main/coexist.c` (a source define — `idf.py -D` sets CMake cache vars, not C macros, so it must be in source, exactly like `CHURN_SELFTEST`) and rebuild:
```powershell
idf.py -DSIMULACRA_BOARD_XIAO_C6=1 build
```
Expected: `Project build complete`. **Remove the temporary `#define`** before committing (ship with no LED pin unless the board's LED wiring is confirmed).

- [ ] **Step 3: Commit**

```bash
git add main/coexist.c
git commit -m "feat(m9): optional board-gated threat LED (SIMULACRA_DETECT_LED_GPIO)"
```

---

## Task 10: Hardware acceptance

**Files:** none (verification only). Uses a carried stable-MAC beacon + serial capture.

- [ ] **Step 1: Confirm the full self-test still passes on both personas**

Set `CHURN_SELFTEST 1`; build+flash+read C6, then C5. Expected both: `SELFTEST: PASS (N/N) fails=0` including all seven `test_detect_*` groups. Revert to `0`.

- [ ] **Step 2: Flash the production decoy (detection on) and prepare the rig**

Flash the default build (C6/XIAO on COM11). Prepare a **stable-MAC beacon** to carry alongside the node: a fixed iBeacon (a phone app, or a spare board pinned to a single non-rotating identity). To make epochs observable in minutes, temporarily shorten the re-profile cadence: set `.reprofile_period_ms` to `5000` in the persona struct in `main/coexist.c` (the M8 acceptance value) — **revert before the final commit**.

- [ ] **Step 3: Drive the three-location test**

Read serial filtered to `THREAT|epoch|reprofile|drift`. Move the node across **2–3 genuinely distinct RF environments** (rooms / streets), keeping the beacon with it.
Expected sequence:
- `reprofile: drift=… ` then `epoch -> 1 (drift=…)`, `epoch -> 2 …`, `epoch -> 3 …` as each environment changes (`drift > 0.45`).
- After the beacon earns presence in the 3rd distinct epoch: `THREAT confirmed id=… vendor=… epochs=3 rssi=… mac=…` (the carried beacon's MAC), then `THREAT persisted id=… rc=0`.
- Moving the node relative to the beacon → throttled `THREAT locate id=… rssi=…` updates.

- [ ] **Step 4: Verify the negatives**

- Ambient devices seen at only **one** location must **not** produce `THREAT confirmed`.
- Over a long stationary run, **no** `THREAT` line names one of our own decoy MACs (self-exclusion holds).
- The combined BLE + Wi-Fi decoy keeps running throughout (`decoy alive active=…`, `burst … tx_rc=0`), no watchdog reset.

- [ ] **Step 5: Restore production settings and reflash**

Revert the temporary `.reprofile_period_ms` back to the persona default (Ward 600000 / Shade 300000). Rebuild + reflash all boards to the production decoy. Confirm the working tree is clean (no temp toggles left).

- [ ] **Step 6: Commit any doc/README updates from acceptance**

If acceptance surfaced README/scope wording (the honest-ceiling note), update `README.md` and commit:
```bash
git add README.md
git commit -m "docs(m9): README — Threat Radar (behavioral follower detection) + honest ceiling"
```

---

## Self-Review

**1. Spec coverage:**
- §1 modules (`detect.{h,c}`, observe callback, coexist adapter, serial+LED, default-on/toggle) → Tasks 1, 6, 7, 8, 9. ✓
- §2 store/data-discipline (per-install salt, RAM candidates hashed + LRU, confirmed→NVS, live-MAC only for confirmed, self-exclusion) → Tasks 1 (candidates), 4 (self-exclude), 5 (salt+NVS), 7 (live-MAC emitted at confirm; hash-only core). ✓
- §3 location-epochs + 3-strike/presence (`DETECT_EPOCH_DRIFT=0.45` separate + both personas, immediate crediting, age-out) → Tasks 1, 2, 7. ✓
- §4 alerting (`THREAT confirmed`/`locate`, RSSI-throttle, LED board-gated, no flash in callback via drain, toggle) → Tasks 3, 7, 9, 8. ✓
- §5 testing (seven `test_detect_*`) + hardware acceptance + scope notes → Tasks 1–5 (tests), 10 (acceptance). ✓
- No spec requirement left without a task.

**2. Placeholder scan:** No TBD/TODO; every code step shows complete code; every test step shows the assertions; every command has an expected result. ✓

**3. Type consistency:** `detect_observe(uint32_t,int8_t,uint16_t,uint16_t)`, `detect_result_t {NONE,CONFIRM,KNOWN}`, `detect_threat_t{hash,vendor,epochs,best_rssi,first_epoch,last_epoch}`, `detect_mac_in_set(mac,set[][6],n)`, `detect_locate_due(rssi,last_rssi,now,last)`, `detect_drain_pending(detect_threat_t*)`, `observe_report_cb_t(mac,rssi,company)`, `coexist_current_epoch()` — all used consistently across Tasks 1–8. NVS namespace `"splinter"` matches the existing `rf_model.c` convention. `identity.addr[6]` (from `identity.h:7`) and `churn_active_at`/`churn_active_count` (from `churn.h:32-33`) used as-declared. ✓

---

## Execution notes

- **Toolchain gotchas** (carry from M8): build with **Python 3.12** on PATH; C5 uses **v5.5**, C6 uses **v5.4**; suppress the export preamble; XIAO C6 needs `-DSIMULACRA_BOARD_XIAO_C6=1`; the C5 reader uses **COM12** (CH343, pulse RTS/DTR to catch boot), the XIAO C6 reader uses **COM11** (native USB-JTAG, no-reset reader; needs a **data** USB cable).
- **`idf.py -D` sets CMake cache vars, not C macros** — flip `CHURN_SELFTEST`, `SIMULACRA_DETECT`, and (for the Task 9 LED test) `SIMULACRA_DETECT_LED_GPIO` in source, never on the command line. `-DSIMULACRA_BOARD_XIAO_C6=1` is the one exception the build system already forwards.
- **Keep `CHURN_SELFTEST 0` and the production `.reprofile_period_ms` in every committed state** — self-test and 5 s cadence are temporary run-time toggles only.
