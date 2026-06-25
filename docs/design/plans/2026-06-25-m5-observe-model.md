# M5 Observe → Model Implementation Plan



**Goal:** Add a BLE-only "observe → model" capability: passively scan ambient advertisements,
extract features, hash-and-drop identifiers at capture, aggregate into a small bounded model of
distributions, and persist that model (never raw captures) to NVS — verified on hardware.

**Architecture:** Two new modules. `rf_model.{h,c}` owns the `rf_model_t` distribution schema, pure
aggregation helpers, NVS save/load, and a serial dump — no NimBLE dependency, fully unit-testable.
`observe.{h,c}` owns the NimBLE passive scan, the disc callback, feature extraction, a salted
ephemeral dedup table (RAM-only, wiped each sweep), and the sweep/persist cadence. A new
`SIMULACRA_OBSERVE` build flag in `simulacra_main.c` selects the live observe loop; the default build
is still the M4 decoy. The pure logic of both modules is exercised by the existing looped on-target
self-test.

**Tech Stack:** ESP-IDF v5.4.4, NimBLE (`ble_gap_disc` / `ble_hs_adv_parse_fields`), NVS, C.
Verified by `idf.py build` + the looped on-target self-test (`CHURN_SELFTEST=1`) and a live serial
dump in the observe build (`SIMULACRA_OBSERVE=1`). Target: ESP32-C6 on COM11.

**Spec:** `docs/design/specs/2026-06-25-m5-observe-model-design.md`

## Global Constraints

- **Aggregates only / hash-and-drop at capture.** Raw MAC, name, and payload are reduced to
  features and **dropped immediately**; only histograms/counts are persisted. `rf_model_t` has **no**
  `addr`/name/payload field — the guarantee is structural.
- **Salted ephemeral dedup.** MACs are hashed with a per-boot random salt (never stored), used only
  for in-RAM dedup; the ephemeral table is wiped at every sweep boundary.
- **Raw-capture dumps behind a compile-time debug flag that does not ship** (not built in M5).
- **BLE-only, dedicated observe mode.** Observe mode never advertises. The default (shipped) build
  is the unchanged M4 decoy population.
- **Bounded model:** 24 vendor slots + `other`, 7 interval bins/vendor, 8 RSSI bins, 5 PDU bins.
  Total < ~1 KB; a single versioned NVS blob.
- Files < 500 lines, one responsibility each.

### Reusable recipes (reuse the M3/M4 recipes verbatim)

**BUILD-RECIPE**, **FLASH-RECIPE**, **SELFTEST-READ** exactly as in
`docs/design/plans/2026-06-24-m3-churn-engine.md` (Python 3.12 ahead of PATH, `. export.ps1`,
C6 on COM11, `simulacra_read.py`). Self-test gated by `#define CHURN_SELFTEST 1`; observe gated by
`#define SIMULACRA_OBSERVE 1` (Tasks 1–3 use the self-test build; Task 4 uses the observe build, then
restores both to 0 to ship the decoy).

> **RED on embedded:** for new pure functions, RED is a **link error** (test references a function
> not yet defined) — the build fails, so there is nothing to flash. GREEN = implement → build →
> flash → SELFTEST-READ shows `PASS (fails=0)`.

---

### Task 1: `rf_model` schema + histogram aggregation

**Files:**
- Create: `main/rf_model.h`, `main/rf_model.c`
- Modify: `main/CMakeLists.txt` (add `rf_model.c`)
- Modify: `main/churn_selftest.c` (add `#include "rf_model.h"` + `test_rf_model()`)

**Interfaces:**
- Produces: `rf_model_t`, `rf_model_reset`, `rf_model_observe`, `rf_model_end_sweep`,
  `rf_itvl_bin`, `rf_rssi_bin`, `rf_pdu_bin`, `rf_vendor_index`, `rf_model_dump`
  (NVS funcs added in Task 3).

- [ ] **Step 1: Create `main/rf_model.h`**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RF_MODEL_MAGIC    0x52464D31u   // "RFM1"
#define RF_MODEL_VERSION  1
#define RF_VENDOR_SLOTS   24
#define RF_ITVL_BINS      7    // <50,50-100,100-200,200-500,500-1000,1000-2000,>2000 ms
#define RF_RSSI_BINS      8    // -100..-20 dBm in 10 dBm steps
#define RF_PDU_BINS       5    // ADV_IND,DIR_IND,SCAN_IND,NONCONN_IND,SCAN_RSP (NimBLE evtype 0..4)
#define RF_VENDOR_UNKNOWN 0xFFFF  // no mfg-data / unknown company id

typedef struct {
    uint16_t company_id;
    uint32_t count;
    uint32_t itvl_bins[RF_ITVL_BINS];
} rf_vendor_t;

typedef struct {
    uint32_t    magic;
    uint16_t    version;
    uint32_t    sweeps;
    uint32_t    total_obs;
    rf_vendor_t vendors[RF_VENDOR_SLOTS];
    uint32_t    other_count;
    uint32_t    other_itvl_bins[RF_ITVL_BINS];
    uint32_t    rssi_bins[RF_RSSI_BINS];
    uint32_t    pdu_bins[RF_PDU_BINS];
    float       pop_ewma;          // EWMA of distinct devices per sweep
    float       arrival_per_min;   // EWMA of new distinct devices per minute
} rf_model_t;

void   rf_model_reset(rf_model_t *m);
// Fold one observation's static features. interval_ms < 0 => no interval sample (first sighting).
void   rf_model_observe(rf_model_t *m, uint16_t company_id, int8_t rssi,
                        uint8_t pdu_type, int32_t interval_ms);
// Fold a completed sweep's distinct-device aggregates (EWMA).
void   rf_model_end_sweep(rf_model_t *m, uint32_t distinct_devices, uint32_t window_ms,
                          uint32_t arrivals);

size_t rf_itvl_bin(int32_t ms);
size_t rf_rssi_bin(int8_t rssi);
size_t rf_pdu_bin(uint8_t pdu_type);
int    rf_vendor_index(const rf_model_t *m, uint16_t company_id);  // occupied slot or -1

void   rf_model_dump(const rf_model_t *m);
```

- [ ] **Step 2: Create `main/rf_model.c`**

```c
#include <string.h>
#include "rf_model.h"
#include "esp_log.h"

static const char *TAG = "rf_model";

void rf_model_reset(rf_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->magic = RF_MODEL_MAGIC;
    m->version = RF_MODEL_VERSION;
}

size_t rf_itvl_bin(int32_t ms)
{
    if (ms < 50)   return 0;
    if (ms < 100)  return 1;
    if (ms < 200)  return 2;
    if (ms < 500)  return 3;
    if (ms < 1000) return 4;
    if (ms < 2000) return 5;
    return 6;
}

size_t rf_rssi_bin(int8_t rssi)
{
    int idx = (rssi + 100) / 10;          // -100 -> 0, -20 -> 8
    if (idx < 0) idx = 0;
    if (idx >= RF_RSSI_BINS) idx = RF_RSSI_BINS - 1;
    return (size_t)idx;
}

size_t rf_pdu_bin(uint8_t pdu_type)
{
    return (pdu_type < RF_PDU_BINS) ? pdu_type : (RF_PDU_BINS - 1);
}

int rf_vendor_index(const rf_model_t *m, uint16_t company_id)
{
    for (size_t i = 0; i < RF_VENDOR_SLOTS; i++)
        if (m->vendors[i].count > 0 && m->vendors[i].company_id == company_id) return (int)i;
    return -1;
}

void rf_model_observe(rf_model_t *m, uint16_t company_id, int8_t rssi,
                      uint8_t pdu_type, int32_t interval_ms)
{
    m->total_obs++;
    m->rssi_bins[rf_rssi_bin(rssi)]++;
    m->pdu_bins[rf_pdu_bin(pdu_type)]++;

    int vi = rf_vendor_index(m, company_id);
    if (vi < 0) {                          // claim a free slot if any
        for (size_t i = 0; i < RF_VENDOR_SLOTS; i++) {
            if (m->vendors[i].count == 0) { m->vendors[i].company_id = company_id; vi = (int)i; break; }
        }
    }
    if (vi >= 0) {
        m->vendors[vi].count++;
        if (interval_ms >= 0) m->vendors[vi].itvl_bins[rf_itvl_bin(interval_ms)]++;
    } else {                               // table full -> overflow bucket
        m->other_count++;
        if (interval_ms >= 0) m->other_itvl_bins[rf_itvl_bin(interval_ms)]++;
    }
}

void rf_model_end_sweep(rf_model_t *m, uint32_t distinct_devices, uint32_t window_ms,
                        uint32_t arrivals)
{
    m->sweeps++;
    float pop = (float)distinct_devices;
    float arr = window_ms ? ((float)arrivals * 60000.0f / (float)window_ms) : 0.0f;
    const float a = 0.3f;                  // EWMA weight
    if (m->sweeps == 1) { m->pop_ewma = pop; m->arrival_per_min = arr; }
    else {
        m->pop_ewma        = a * pop + (1.0f - a) * m->pop_ewma;
        m->arrival_per_min = a * arr + (1.0f - a) * m->arrival_per_min;
    }
}

void rf_model_dump(const rf_model_t *m)
{
    // Print floats as rounded integers — avoids the newlib-nano "%f" pitfall on the C6.
    ESP_LOGW(TAG, "MODEL v%u sweeps=%u obs=%u pop=%u arr/min=%u other=%u",
             m->version, (unsigned)m->sweeps, (unsigned)m->total_obs,
             (unsigned)(m->pop_ewma + 0.5f), (unsigned)(m->arrival_per_min + 0.5f),
             (unsigned)m->other_count);
    for (size_t i = 0; i < RF_VENDOR_SLOTS; i++) {
        if (m->vendors[i].count == 0) continue;
        const rf_vendor_t *v = &m->vendors[i];
        ESP_LOGW(TAG, "  vendor 0x%04X n=%u itvl[%u %u %u %u %u %u %u]",
                 v->company_id, (unsigned)v->count,
                 (unsigned)v->itvl_bins[0], (unsigned)v->itvl_bins[1], (unsigned)v->itvl_bins[2],
                 (unsigned)v->itvl_bins[3], (unsigned)v->itvl_bins[4], (unsigned)v->itvl_bins[5],
                 (unsigned)v->itvl_bins[6]);
    }
    ESP_LOGW(TAG, "  rssi[%u %u %u %u %u %u %u %u] pdu[%u %u %u %u %u]",
             (unsigned)m->rssi_bins[0], (unsigned)m->rssi_bins[1], (unsigned)m->rssi_bins[2],
             (unsigned)m->rssi_bins[3], (unsigned)m->rssi_bins[4], (unsigned)m->rssi_bins[5],
             (unsigned)m->rssi_bins[6], (unsigned)m->rssi_bins[7],
             (unsigned)m->pdu_bins[0], (unsigned)m->pdu_bins[1], (unsigned)m->pdu_bins[2],
             (unsigned)m->pdu_bins[3], (unsigned)m->pdu_bins[4]);
}
```

- [ ] **Step 3: Add the self-test** — in `main/churn_selftest.c`, add `#include "rf_model.h"` near the
top, then add this test and call it from `churn_selftest_run()` after the M4 tests:

```c
static void test_rf_model(void)
{
    rf_model_t m;
    rf_model_reset(&m);
    ST_CHECK(m.magic == RF_MODEL_MAGIC && m.version == RF_MODEL_VERSION, "rf_model resets to valid header");
    ST_CHECK(m.total_obs == 0 && m.other_count == 0, "rf_model resets counts to zero");

    // bin edges
    ST_CHECK(rf_itvl_bin(49) == 0 && rf_itvl_bin(50) == 1 && rf_itvl_bin(2001) == 6, "interval bin edges");
    ST_CHECK(rf_rssi_bin(-100) == 0 && rf_rssi_bin(-20) == 7, "rssi bin edges");
    ST_CHECK(rf_pdu_bin(0) == 0 && rf_pdu_bin(4) == 4 && rf_pdu_bin(9) == RF_PDU_BINS - 1, "pdu bin map");

    // first sighting (no interval), then a repeat with a 150 ms interval
    rf_model_observe(&m, 0x004C, -50, 0, -1);
    rf_model_observe(&m, 0x004C, -50, 0, 150);
    int vi = rf_vendor_index(&m, 0x004C);
    ST_CHECK(vi >= 0 && m.vendors[vi].count == 2, "vendor slot counts both observations");
    ST_CHECK(m.vendors[vi].itvl_bins[rf_itvl_bin(150)] == 1, "interval sample lands in bin 2");
    ST_CHECK(m.vendors[vi].itvl_bins[0] == 0, "no spurious interval sample for first sighting");
    ST_CHECK(m.rssi_bins[rf_rssi_bin(-50)] == 2 && m.pdu_bins[0] == 2, "rssi+pdu histograms updated");

    // vendor-table overflow -> other bucket
    rf_model_t o; rf_model_reset(&o);
    for (int i = 0; i < RF_VENDOR_SLOTS; i++) rf_model_observe(&o, (uint16_t)(0x100 + i), -60, 0, -1);
    rf_model_observe(&o, 0x9999, -60, 0, -1);   // 25th distinct vendor
    ST_CHECK(o.other_count == 1, "overflow vendor lands in other bucket");

    // sweep EWMA
    rf_model_t s; rf_model_reset(&s);
    rf_model_end_sweep(&s, 5, 60000, 5);
    ST_CHECK((int)(s.pop_ewma + 0.5f) == 5 && (int)(s.arrival_per_min + 0.5f) == 5, "first sweep seeds EWMA");
}
```
Add `test_rf_model();` in `churn_selftest_run()` (after `test_roster_payloads();`).

- [ ] **Step 4: Register `rf_model.c`** — `main/CMakeLists.txt`:
```cmake
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c" "churn.c" "templates.c" "rf_model.c"
```

- [ ] **Step 5: RED then GREEN.** Set `#define CHURN_SELFTEST 1` in `simulacra_main.c` (leave
`SIMULACRA_OBSERVE` absent/0). Before writing `rf_model.c`, a build would link-fail (RED). With
`rf_model.c` present: BUILD-RECIPE → FLASH-RECIPE → SELFTEST-READ → `SELFTEST result: PASS (fails=0)`.

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/rf_model.h main/rf_model.c main/CMakeLists.txt main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m5): rf_model distribution schema + histogram aggregation"
```

---

### Task 2: Ephemeral dedup + interval/population/arrival + salted hash

**Files:**
- Create: `main/observe.h`, `main/observe.c`
- Modify: `main/CMakeLists.txt` (add `observe.c`)
- Modify: `main/churn_selftest.c` (add `#include "observe.h"` + `test_observe_dedup()`)

**Interfaces:**
- Consumes Task 1 `rf_model_*`.
- Produces: `observe_reset_ephemeral`, `observe_hash_mac`, `observe_ingest`, `observe_end_sweep`,
  `observe_ephemeral_count` (radio funcs `observe_start`/event handler added in Task 4).

- [ ] **Step 1: Create `main/observe.h`**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "rf_model.h"

// --- capture-side primitives (pure; unit-tested without the radio) ---

// Wipe the ephemeral dedup table and set the per-boot hashing salt (never persisted).
void     observe_reset_ephemeral(uint32_t boot_salt);
// Salted FNV-1a over the 6-byte MAC. The MAC is read here and never stored.
uint32_t observe_hash_mac(const uint8_t mac[6]);
// Ingest one feature-extracted observation: hash the MAC for dedup, estimate the interval
// from the last sighting, and fold features into the model. The raw MAC is dropped.
void     observe_ingest(rf_model_t *m, const uint8_t mac[6], uint32_t now_ms,
                        uint16_t company_id, int8_t rssi, uint8_t pdu_type);
// Close the current sweep window: fold distinct-device count + arrivals into the model
// (EWMA) and wipe the ephemeral table.
void     observe_end_sweep(rf_model_t *m, uint32_t window_ms);
// Distinct hashes currently held in the ephemeral table (for tests/heartbeat).
size_t   observe_ephemeral_count(void);

// --- live radio path (implemented in Task 4) ---
void     observe_start(uint32_t boot_salt);   // load model from NVS, start passive scan
```

- [ ] **Step 2: Create `main/observe.c`** (capture-side only for now; radio path added in Task 4)

```c
#include <string.h>
#include "observe.h"

#define OBS_TABLE_CAP 256

typedef struct { uint32_t hash; uint32_t first_ms; uint32_t last_ms; bool used; } obs_entry_t;

static obs_entry_t s_tbl[OBS_TABLE_CAP];
static uint32_t    s_salt;
static uint32_t    s_arrivals;     // new distinct hashes this window
static bool        s_saturated;

void observe_reset_ephemeral(uint32_t boot_salt)
{
    memset(s_tbl, 0, sizeof(s_tbl));
    s_salt = boot_salt;
    s_arrivals = 0;
    s_saturated = false;
}

uint32_t observe_hash_mac(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_salt;     // FNV-1a offset basis, salted
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

void observe_ingest(rf_model_t *m, const uint8_t mac[6], uint32_t now_ms,
                    uint16_t company_id, int8_t rssi, uint8_t pdu_type)
{
    uint32_t h = observe_hash_mac(mac);    // MAC consumed here, never stored
    int32_t interval = -1;
    obs_entry_t *slot = NULL, *freep = NULL;
    for (size_t i = 0; i < OBS_TABLE_CAP; i++) {
        if (s_tbl[i].used && s_tbl[i].hash == h) { slot = &s_tbl[i]; break; }
        if (!s_tbl[i].used && !freep) freep = &s_tbl[i];
    }
    if (slot) {
        interval = (int32_t)(now_ms - slot->last_ms);
        slot->last_ms = now_ms;
    } else if (freep) {
        freep->used = true; freep->hash = h; freep->first_ms = now_ms; freep->last_ms = now_ms;
        s_arrivals++;
    } else {
        s_saturated = true;                // full: still counted in the model, just not deduped
    }
    rf_model_observe(m, company_id, rssi, pdu_type, interval);
}

void observe_end_sweep(rf_model_t *m, uint32_t window_ms)
{
    uint32_t distinct = 0;
    for (size_t i = 0; i < OBS_TABLE_CAP; i++) if (s_tbl[i].used) distinct++;
    rf_model_end_sweep(m, distinct, window_ms, s_arrivals);
    memset(s_tbl, 0, sizeof(s_tbl));       // wipe ephemeral identifiers
    s_arrivals = 0;
    s_saturated = false;
}

size_t observe_ephemeral_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < OBS_TABLE_CAP; i++) if (s_tbl[i].used) n++;
    return n;
}
```

- [ ] **Step 3: Register `observe.c`** — `main/CMakeLists.txt` SRCS: append `"observe.c"`.
Also add `REQUIRES`/`PRIV_REQUIRES` note: `observe.c` needs `bt` (already required) for Task 4; no
change needed now.

- [ ] **Step 4: Add the self-test** — in `churn_selftest.c`, add `#include "observe.h"` and:

```c
static void test_observe_dedup(void)
{
    const uint8_t A[6] = {0x01,0x02,0x03,0x04,0x05,0xC0};
    const uint8_t B[6] = {0x11,0x12,0x13,0x14,0x15,0xC1};
    rf_model_t m; rf_model_reset(&m);
    observe_reset_ephemeral(0xDEADBEEF);

    observe_ingest(&m, A, 1000, 0x0075, -40, 0);   // first sighting of A
    observe_ingest(&m, A, 1150, 0x0075, -40, 0);   // repeat -> 150 ms interval
    ST_CHECK(observe_ephemeral_count() == 1, "repeat MAC does not inflate distinct count");
    int vi = rf_vendor_index(&m, 0x0075);
    ST_CHECK(vi >= 0 && m.vendors[vi].itvl_bins[rf_itvl_bin(150)] == 1, "interval estimated from re-sighting");

    observe_ingest(&m, B, 1200, 0x0087, -55, 0);   // a second distinct device
    ST_CHECK(observe_ephemeral_count() == 2, "distinct devices counted");

    observe_end_sweep(&m, 60000);
    ST_CHECK((int)(m.pop_ewma + 0.5f) == 2, "sweep population = distinct devices");
    ST_CHECK((int)(m.arrival_per_min + 0.5f) == 2, "arrival rate folded");
    ST_CHECK(observe_ephemeral_count() == 0, "ephemeral table wiped after sweep (no identifiers retained)");

    // salt sensitivity: the same MAC hashes differently under a different per-boot salt,
    // so the in-RAM hash is not a stable cross-boot identifier.
    observe_reset_ephemeral(0x11111111u); uint32_t h1 = observe_hash_mac(A);
    observe_reset_ephemeral(0x22222222u); uint32_t h2 = observe_hash_mac(A);
    ST_CHECK(h1 != h2, "salt makes the dedup hash non-stable across boots");
}
```
Call `test_observe_dedup();` in `churn_selftest_run()` (after `test_rf_model();`).

- [ ] **Step 5: RED then GREEN.** `CHURN_SELFTEST 1`. RED = link error before `observe.c` exists.
GREEN: BUILD-RECIPE → FLASH-RECIPE → SELFTEST-READ → `PASS (fails=0)`.

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/observe.h main/observe.c main/CMakeLists.txt main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m5): salted ephemeral dedup + interval/population/arrival aggregation"
```

---

### Task 3: NVS persistence (versioned blob) + round-trip self-test

**Files:** Modify `main/rf_model.h` (declare save/load), `main/rf_model.c` (implement),
`main/churn_selftest.c` (round-trip test).

- [ ] **Step 1: Declare in `rf_model.h`** (after `rf_model_dump`):
```c
// NVS persistence: namespace "splinter", key "rf_model". Return 0 on success.
int rf_model_save_nvs(const rf_model_t *m);
int rf_model_load_nvs(rf_model_t *m);   // 0 and fills m if a valid current-version blob exists
```

- [ ] **Step 2: Implement in `rf_model.c`** (add `#include "nvs.h"` at the top):
```c
int rf_model_save_nvs(const rf_model_t *m)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READWRITE, &h);
    if (e != ESP_OK) return (int)e;
    e = nvs_set_blob(h, "rf_model", m, sizeof(*m));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (int)e;
}

int rf_model_load_nvs(rf_model_t *m)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READONLY, &h);
    if (e != ESP_OK) return (int)e;
    size_t len = sizeof(*m);
    e = nvs_get_blob(h, "rf_model", m, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(*m) ||
        m->magic != RF_MODEL_MAGIC || m->version != RF_MODEL_VERSION) return -1;
    return 0;
}
```

- [ ] **Step 3: Add the self-test** — in `churn_selftest.c`:
```c
static void test_rf_model_nvs(void)
{
    rf_model_t m; rf_model_reset(&m);
    rf_model_observe(&m, 0x004C, -50, 0, 150);
    rf_model_observe(&m, 0x0075, -60, 3, 900);
    rf_model_end_sweep(&m, 3, 60000, 3);

    ST_CHECK(rf_model_save_nvs(&m) == 0, "rf_model saves to NVS");
    rf_model_t r; memset(&r, 0xAA, sizeof(r));
    ST_CHECK(rf_model_load_nvs(&r) == 0, "rf_model loads from NVS");
    ST_CHECK(memcmp(&m, &r, sizeof(m)) == 0, "rf_model NVS round-trips byte-exact");
}
```
Call `test_rf_model_nvs();` in `churn_selftest_run()` (after `test_observe_dedup();`).

- [ ] **Step 4: RED then GREEN.** `CHURN_SELFTEST 1`. RED = link error before the NVS funcs exist.
GREEN: BUILD-RECIPE → FLASH-RECIPE → SELFTEST-READ → `PASS (fails=0)`.
(NVS is initialized in `app_main`; the self-test runs after that, so the handle opens cleanly.)

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/rf_model.h main/rf_model.c main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m5): versioned NVS persistence for rf_model + round-trip test"
```

---

### Task 4: Live passive scan + observe loop + serial dump + hardware acceptance

**Files:** Modify `main/observe.h` (already declares `observe_start`), `main/observe.c` (scan +
callback + sweep/persist cadence), `main/simulacra_main.c` (`SIMULACRA_OBSERVE` flag + observe branch),
`README.md` (observe-mode note).

**Interfaces:** Consumes Tasks 1–3. Produces the live `observe_start` + internal gap callback.

- [ ] **Step 1: Add the radio path to `main/observe.c`.** Add includes at the top:
```c
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
```
Add the sweep/persist tunables, the persistent model, and the live scan below the capture-side code:
```c
static const char *TAG = "observe";
#define OBS_SWEEP_MS       15000   // observation window (15 s: short enough for the ~13 s reader)
#define OBS_PERSIST_EVERY  1       // persist + dump every N sweeps

static rf_model_t s_model;
static uint32_t   s_sweep_start_ms;
static uint32_t   s_persist_ctr;

static void observe_maybe_close_sweep(uint32_t now)
{
    if (now - s_sweep_start_ms < OBS_SWEEP_MS) return;
    observe_end_sweep(&s_model, now - s_sweep_start_ms);
    s_sweep_start_ms = now;
    ESP_LOGW(TAG, "[sweep %u] pop=%u arr/min=%u obs=%u",
             (unsigned)s_model.sweeps, (unsigned)(s_model.pop_ewma + 0.5f),
             (unsigned)(s_model.arrival_per_min + 0.5f), (unsigned)s_model.total_obs);
    if (++s_persist_ctr >= OBS_PERSIST_EVERY) {
        s_persist_ctr = 0;
        rf_model_save_nvs(&s_model);
        rf_model_dump(&s_model);
    }
}

static int observe_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {        // restart if the scan ever ends
        struct ble_gap_disc_params p; memset(&p, 0, sizeof(p)); p.passive = 1;
        ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, observe_gap_event, NULL);
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_gap_disc_desc *d = &event->disc;
    uint16_t company = RF_VENDOR_UNKNOWN;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 &&
        f.mfg_data && f.mfg_data_len >= 2)
        company = (uint16_t)(f.mfg_data[0] | (f.mfg_data[1] << 8));

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    observe_ingest(&s_model, d->addr.val, now, company, d->rssi, d->event_type);  // MAC dropped inside
    observe_maybe_close_sweep(now);
    return 0;
}

void observe_start(uint32_t boot_salt)
{
    observe_reset_ephemeral(boot_salt);
    if (rf_model_load_nvs(&s_model) != 0) rf_model_reset(&s_model);
    s_sweep_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_persist_ctr = 0;

    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    p.passive = 1;                 // never send scan requests -> never reveal ourselves
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, observe_gap_event, NULL);
    ESP_LOGW(TAG, "observe scan start rc=%d", rc);
}
```
> All `s_model` access happens in the NimBLE host-task context (the disc callback), so there is no
> cross-task sharing and no lock is needed. `simulacra_task` only kicks off the scan and idles.

- [ ] **Step 2: Wire the observe mode in `main/simulacra_main.c`.** Add the include and flag:
```c
#include "observe.h"
```
```c
#ifndef SIMULACRA_OBSERVE
#define SIMULACRA_OBSERVE 0
#endif
```
Replace the `#if CHURN_SELFTEST ... #else ... #endif` block in `simulacra_task` with:
```c
#if SIMULACRA_OBSERVE
    observe_start(esp_random());
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }   // scanning + modeling run in the host callback
#elif CHURN_SELFTEST
    int fails = churn_selftest_run();
    for (;;) {
        ESP_LOGW(TAG, "SELFTEST result: %s (fails=%d)", fails ? "FAIL" : "PASS", fails);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    roster_init();
    churn_set_apply(churn_adv_apply);
    churn_init((uint32_t)(esp_timer_get_time() / 1000));
    for (;;) {
        churn_tick((uint32_t)(esp_timer_get_time() / 1000));
        vTaskDelay(pdMS_TO_TICKS(CHURN_TICK_MS));
    }
#endif
```

- [ ] **Step 3: Build + flash the observe build.** Set `#define SIMULACRA_OBSERVE 1` and
`#define CHURN_SELFTEST 0` in `simulacra_main.c`. BUILD-RECIPE → FLASH-RECIPE.

- [ ] **Step 4: Hardware acceptance read.** With normal BLE traffic in range (phone, watch, the
earlier real beacons), SELFTEST-READ the COM11 port. Run it 2–3 times across ~45 s. Expect:
  - `observe scan start rc=0`
  - per-sweep heartbeats `[sweep N] pop=.. arr/min=.. obs=..` with non-zero `obs`
  - a `MODEL ...` dump listing real vendor company IDs with counts, plus rssi/pdu histograms
  - **no** `task_wdt`, no panic/reboot
  Acceptance = the dump is all distributions/counts (vendors, intervals, rssi, pdu, pop) with **no
  per-device identifiers anywhere** (verify by inspection — there is no MAC/name field to print).

- [ ] **Step 5: Restore the shipped decoy.** Set `#define SIMULACRA_OBSERVE 0` (CHURN_SELFTEST already
0) in `simulacra_main.c`. BUILD-RECIPE → FLASH-RECIPE. Quick SELFTEST-READ: silent (decoy running, no
`rc=` warnings) confirms the default device is back to the M4 population.

- [ ] **Step 6: Docs.** In `README.md`, add a short "Observe mode (M5)" subsection under
Configuration: build with `SIMULACRA_OBSERVE=1` to profile the ambient BLE environment into an
NVS-stored model of distributions (vendor mix, per-vendor intervals, RSSI spread, population,
arrival rate); it never advertises and stores **no per-device identifiers** (MACs hashed-and-dropped
at capture). Note the default build is unchanged (the decoy). Mention `OBS_SWEEP_MS` /
`OBS_PERSIST_EVERY` in `observe.c` as the cadence knobs (raise from the 15 s test value for
deployment).

- [ ] **Step 7: Commit**
```bash
git -C "~/Downloads/simulacra" add main/observe.h main/observe.c main/simulacra_main.c README.md
git -C "~/Downloads/simulacra" commit -m "feat(m5): live passive-scan observe loop + serial model dump; docs"
```

---

## Acceptance (M5, from the spec)

- After a sweep, the stored model contains distributions and counts and **no** recoverable
  per-device identifiers (verified by inspecting the dump — `rf_model_t` has no identifier field).
  ✅ Task 4 Step 4 + the structural schema.
- Hash-and-drop at capture; ephemeral table wiped each sweep; salt makes the dedup hash non-stable.
  ✅ self-test (Task 2).
- Model updates on a slow cadence and persists to NVS. ✅ Task 3 + Task 4 cadence.
- Each milestone leaves a flashable working device: default build is the unchanged M4 decoy.
  ✅ Task 4 Step 5.
```
