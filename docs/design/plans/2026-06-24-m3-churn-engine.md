# M3 Churn Engine Implementation Plan



**Goal:** Replace Splinter v1's single rotating legacy advertiser with a synthetic BLE *population* — ~8 persistent, reappear-after-cooldown identities time-sliced across the C6's 4 extended-advertising instances.

**Architecture:** A pure churn state machine (`churn.c`) takes an injected `now_ms` clock and an injected `apply(instance, identity)` callback; it owns a fixed active set + occupant table and drives a `roster.c` pool of pre-generated persistent identities through `IDLE → ACTIVE → COOLDOWN → IDLE`. A NimBLE adapter (`churn_adv.c`) implements the production `apply` via `ble_gap_ext_adv_*`. An on-target self-test (`churn_selftest.c`) drives the state machine with a fake clock + a recording `apply`, asserts invariants, and loop-prints PASS/FAIL over serial. `simulacra_main.c` either runs the self-test (radio off) or the real churn task, by compile flag.

**Tech Stack:** ESP-IDF v5.4.4, NimBLE (extended advertising), FreeRTOS, C. Verification by `idf.py build` + host `bleak` scan + serial read. Target: Seeed XIAO ESP32-C6 on COM11.

**Spec:** `docs/design/specs/2026-06-24-m3-churn-engine-design.md`

## Global Constraints

- **Hardware ceiling:** at most **4** concurrent ext-adv instances on the C6 (`CONFIG_BT_LE_MAX_EXT_ADV_INSTANCES` / `CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES`, `range 0 4`).
- **Non-connectable only:** every advertiser is legacy-PDU, `connectable=0`, `scannable=0`.
- **Keep pairing-popup formats excluded:** payloads come only from the unchanged `build_decoy_fields` / `decoy_vendors.h` (no Apple `0x004C` Continuity, MS `0x0006` Swift Pair, Google `0xFE2C` Fast Pair shapes).
- **Single-core watchdog discipline:** the churn task `vTaskDelay`s every tick; never spin without yielding.
- **No heap:** roster, active set, occupant table are fixed static arrays.
- **Files < 500 lines**, one responsibility each.
- **No verbatim replay / aggregates only:** identities are synthetic; nothing real is captured or persisted (capture is M5).
- **Build env:** Python 3.12 forced ahead of 3.14; ESP-IDF at `~\esp\v5.4\esp-idf`; port **COM11**.

### Reusable recipes (referenced by steps)

**BUILD-RECIPE** (PowerShell — shell state does not persist between calls, so re-source every time):
```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$env:IDF_PATH='~\esp\v5.4\esp-idf'
. ($env:IDF_PATH+'\export.ps1') | Out-Null
Set-Location '~\Downloads\simulacra'
idf.py build
```
**FLASH-RECIPE:** append `idf.py -p COM11 flash` to BUILD-RECIPE (or run after a successful build).

**SELFTEST builds (Tasks 2–5):** the self-test is gated by `#define CHURN_SELFTEST` in `simulacra_main.c` (default 0). Set it to **1** to build/flash the on-target self-test; Task 6 sets it back to 0. (`idf.py -D…` sets CMake cache vars, not C macros, so toggle in source.)

**SELFTEST-READ** (catches the looped self-test summary on COM11):
```powershell
& '~\.espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe' `
  '~\AppData\Local\Temp\simulacra_read.py'
```
`simulacra_read.py` (already present from v1 verification) reads COM11 for ~13 s and prints what it sees.

**BLEAK-SCAN** (integration acceptance):
```powershell
& 'C:\Program Files\Python312\python.exe' '~\AppData\Local\Temp\simulacra_bleak_scan.py'
```
For M3 acceptance this scan script is extended (Task 6) to track per-MAC first/last-seen over a few minutes.

> First re-target only (Task 1) needs `Remove-Item build,sdkconfig -Recurse -Force; idf.py set-target esp32c6` so the new `sdkconfig.defaults.esp32c6` is picked up. After that, plain `idf.py build` is incremental.

---

### Task 1: Flip C6 to extended advertising + NimBLE adapter (4 concurrent instances)

De-risks the hardware/config path before any churn logic: enable EXT_ADV on the C6 and prove 4 genuinely-concurrent ext-adv instances advertise on this controller, via the adapter the churn engine will reuse.

**Files:**
- Create: `sdkconfig.defaults.esp32c6`
- Create: `main/identity.h`
- Create: `main/churn_adv.h`, `main/churn_adv.c`
- Modify: `main/simulacra_main.c` (temporary 4-instance smoke driver in the EXT_ADV path)
- Modify: `main/CMakeLists.txt` (add `churn_adv.c`)

**Interfaces:**
- Produces: `identity_t` (struct), `void churn_adv_apply(uint8_t instance, const identity_t *id)`.

- [ ] **Step 1: Add the C6 EXT_ADV defaults**

Create `sdkconfig.defaults.esp32c6`:
```
# Splinter v2: extended advertising for genuinely-concurrent decoy instances on the C6.
CONFIG_BT_NIMBLE_EXT_ADV=y
CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=4
CONFIG_BT_LE_EXT_ADV=y
CONFIG_BT_LE_MAX_EXT_ADV_INSTANCES=4
```

- [ ] **Step 2: Define `identity_t`**

Create `main/identity.h`:
```c
#pragma once
#include <stdint.h>

typedef enum { ID_IDLE, ID_ACTIVE, ID_COOLDOWN } id_state_t;

typedef struct {
    uint8_t    addr[6];          // stable random-static MAC (top 2 bits set)
    uint16_t   company_id;       // vendor company id (debug/inspection)
    uint8_t    payload[31];      // frozen, serialized AD bytes
    uint8_t    payload_len;
    uint16_t   adv_itvl_ms;      // this identity's on-air interval
    id_state_t state;
    uint32_t   active_until_ms;  // ACTIVE: dwell deadline (absolute ms)
    uint32_t   eligible_at_ms;   // COOLDOWN: earliest re-promotion time (absolute ms)
} identity_t;
```

- [ ] **Step 3: Write the NimBLE ext-adv adapter**

Create `main/churn_adv.h`:
```c
#pragma once
#include <stdint.h>
#include "identity.h"

#define CHURN_HW_INSTANCES 4

// Apply: make hardware `instance` advertise `id` (stop, (re)configure with the
// identity's interval, set random addr, set data, start). Safe to call from the
// churn task after NimBLE host sync.
void churn_adv_apply(uint8_t instance, const identity_t *id);
```

Create `main/churn_adv.c`:
```c
#include <string.h>
#include "churn_adv.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "nimble/hci_common.h"

static const char *TAG = "churn_adv";
#define ADV_ITVL_UNITS(ms) ((uint16_t)(((ms) * 1000) / 625))

void churn_adv_apply(uint8_t instance, const identity_t *id)
{
    int rc;
    ble_gap_ext_adv_stop(instance);  // ok if not running

    struct ble_gap_ext_adv_params p;
    memset(&p, 0, sizeof(p));
    p.legacy_pdu    = 1;             // legacy PDU -> visible to all scanners
    p.connectable   = 0;
    p.scannable     = 0;
    p.own_addr_type = BLE_OWN_ADDR_RANDOM;
    p.primary_phy   = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = BLE_HCI_LE_PHY_1M;
    p.sid           = instance;
    p.itvl_min      = ADV_ITVL_UNITS(id->adv_itvl_ms);
    p.itvl_max      = ADV_ITVL_UNITS(id->adv_itvl_ms + 30);
    p.tx_power      = 127;
    rc = ble_gap_ext_adv_configure(instance, &p, NULL, NULL, NULL);
    if (rc) { ESP_LOGW(TAG, "configure inst %u rc=%d", instance, rc); return; }

    ble_addr_t a;
    a.type = BLE_ADDR_RANDOM;
    memcpy(a.val, id->addr, 6);
    rc = ble_gap_ext_adv_set_addr(instance, &a);
    if (rc) { ESP_LOGW(TAG, "set_addr inst %u rc=%d", instance, rc); return; }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(id->payload, id->payload_len);
    if (!om) { ESP_LOGW(TAG, "mbuf NULL inst %u", instance); return; }
    rc = ble_gap_ext_adv_set_data(instance, om);  // consumes om
    if (rc) { ESP_LOGW(TAG, "set_data inst %u rc=%d", instance, rc); return; }

    rc = ble_gap_ext_adv_start(instance, 0, 0);   // duration 0 = forever
    if (rc) ESP_LOGW(TAG, "start inst %u rc=%d", instance, rc);
}
```

- [ ] **Step 4: Temporary smoke driver — 4 fixed identities**

In `main/simulacra_main.c`, inside the `#ifdef CONFIG_BT_NIMBLE_EXT_ADV` branch, replace the body of `simulacra_run()` with a one-shot driver that builds 4 identities and applies one per instance. Reuse the existing `make_random_static_addr` and `build_decoy_fields`; add at top `#include "identity.h"` and `#include "churn_adv.h"`:
```c
static void fill_identity(identity_t *id)
{
    make_random_static_addr(id->addr);
    struct ble_hs_adv_fields f;
    uint8_t mfg[10];
    build_decoy_fields(&f, mfg);
    uint8_t buf[BLE_HS_ADV_MAX_SZ];
    uint8_t len = 0;
    if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) len = 0;
    memcpy(id->payload, buf, len);
    id->payload_len = len;
    id->adv_itvl_ms = 120;
}

static void simulacra_run(void)
{
    ESP_LOGW(TAG, "M3 Task1 smoke: 4 concurrent ext-adv instances");
    static identity_t ids[CHURN_HW_INSTANCES];
    for (uint8_t i = 0; i < CHURN_HW_INSTANCES; i++) {
        fill_identity(&ids[i]);
        churn_adv_apply(i, &ids[i]);
    }
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
```

- [ ] **Step 5: Register the new source**

In `main/CMakeLists.txt`, add `churn_adv.c` to SRCS:
```cmake
idf_component_register(
    SRCS "simulacra_main.c" "churn_adv.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash
)
```

- [ ] **Step 6: Re-target + build**

Run (PowerShell, with BUILD-RECIPE env active):
```powershell
Remove-Item '~\Downloads\simulacra\build','~\Downloads\simulacra\sdkconfig' -Recurse -Force -ErrorAction SilentlyContinue
idf.py set-target esp32c6
idf.py build
```
Expected: build succeeds. Verify EXT_ADV is on:
```powershell
Select-String -Path '~\Downloads\simulacra\sdkconfig' -Pattern 'CONFIG_BT_NIMBLE_EXT_ADV=y','CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=4'
```
Expected: both lines present.

- [ ] **Step 7: Flash + verify 4 concurrent advertisers**

Run FLASH-RECIPE (`idf.py -p COM11 flash`), then BLEAK-SCAN.
Expected: the scan reports **≥4 distinct decoy MACs** (random-static, top octet ≥ 0xC0) with decoy company IDs from `decoy_vendors.h` present simultaneously.

- [ ] **Step 8: Commit**
```powershell
git -C '~\Downloads\simulacra' add sdkconfig.defaults.esp32c6 main/identity.h main/churn_adv.h main/churn_adv.c main/simulacra_main.c main/CMakeLists.txt
git -C '~\Downloads\simulacra' commit -m "feat(m3): enable ext-adv on C6 + NimBLE adapter, 4 concurrent instances"
```

---

### Task 2: Roster module + on-target self-test harness

Pre-generate the persistent identity pool and stand up the self-test framework (fake clock + recording apply + looped PASS/FAIL print).

**Files:**
- Create: `main/roster.h`, `main/roster.c`
- Create: `main/churn_selftest.h`, `main/churn_selftest.c`
- Modify: `main/simulacra_main.c` (self-test mode behind `CHURN_SELFTEST`)
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `identity_t` (Task 1).
- Produces: `void roster_init(void)`, `identity_t *roster_promote_candidate(uint32_t now_ms)`, `size_t roster_count_in_state(id_state_t s)`; `CHURN_ROSTER_SIZE`. Self-test: `int churn_selftest_run(void)` returns failing-assert count (0 = pass).

- [ ] **Step 1: Roster header + tunables**

Create `main/roster.h`:
```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "identity.h"

#define CHURN_ROSTER_SIZE 256

void       roster_init(void);
identity_t *roster_promote_candidate(uint32_t now_ms);  // eligible identity (now IDLE) or NULL
size_t     roster_count_in_state(id_state_t s);
identity_t *roster_at(size_t i);                        // for tests
```

- [ ] **Step 2: Roster implementation (moves MAC + payload generation here)**

Create `main/roster.c` (relocate `make_random_static_addr` + `build_decoy_fields` out of simulacra_main.c into here, exported for Task 1's smoke driver via roster.h if desired; for this task keep them static in roster.c):
```c
#include <string.h>
#include "roster.h"
#include "decoy_vendors.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

#define SIMULACRA_NAME_PROB 60
#define SIMULACRA_MFG_PROB  85

static identity_t s_roster[CHURN_ROSTER_SIZE];
static size_t     s_cursor;

static void make_random_static_addr(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[5] |= 0xc0;
        int ones = __builtin_popcount(out[5] & 0x3f);
        for (int i = 0; i < 5; i++) ones += __builtin_popcount(out[i]);
        if (ones != 0 && ones != 46) return;
    }
}

static uint16_t build_fields(struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    const vendor_t *v = &VENDORS[esp_random() % VENDOR_COUNT];
    memset(f, 0, sizeof(*f));
    f->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    bool used_name = false;
    if (v->name && (esp_random() % 100) < SIMULACRA_NAME_PROB) {
        f->name = (uint8_t *)v->name; f->name_len = strlen(v->name);
        f->name_is_complete = 1; used_name = true;
    }
    if ((esp_random() % 100) < SIMULACRA_MFG_PROB) {
        size_t body = used_name ? 3 : (3 + (esp_random() % 5));
        mfg[0] = (uint8_t)(v->company_id & 0xff);
        mfg[1] = (uint8_t)((v->company_id >> 8) & 0xff);
        for (size_t i = 0; i < body; i++) mfg[2 + i] = (uint8_t)(esp_random() & 0xff);
        f->mfg_data = mfg; f->mfg_data_len = 2 + body;
    }
    f->tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO; f->tx_pwr_lvl_is_present = 1;
    return v->company_id;
}

void roster_init(void)
{
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = &s_roster[i];
        make_random_static_addr(id->addr);
        struct ble_hs_adv_fields f; uint8_t mfg[10];
        id->company_id = build_fields(&f, mfg);
        uint8_t buf[BLE_HS_ADV_MAX_SZ], len = 0;
        if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) len = 0;
        memcpy(id->payload, buf, len); id->payload_len = len;
        id->adv_itvl_ms = 100 + (esp_random() % 200);  // 100-300 ms
        id->state = ID_IDLE; id->active_until_ms = 0; id->eligible_at_ms = 0;
    }
    s_cursor = 0;
}

identity_t *roster_promote_candidate(uint32_t now_ms)
{
    for (size_t k = 0; k < CHURN_ROSTER_SIZE; k++) {
        size_t i = (s_cursor + k) % CHURN_ROSTER_SIZE;
        identity_t *id = &s_roster[i];
        if (id->state == ID_IDLE ||
            (id->state == ID_COOLDOWN && now_ms >= id->eligible_at_ms)) {
            id->state = ID_IDLE;
            s_cursor = (i + 1) % CHURN_ROSTER_SIZE;
            return id;
        }
    }
    return NULL;
}

size_t roster_count_in_state(id_state_t s)
{
    size_t n = 0;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) if (s_roster[i].state == s) n++;
    return n;
}

identity_t *roster_at(size_t i) { return &s_roster[i]; }
```
> Note: `roster.c` has its **own static** `make_random_static_addr` + `build_fields`. Do **not** remove the copies in `simulacra_main.c` — the Task 1 smoke driver still uses them. The temporary duplication is deleted in Task 6 with the smoke driver. Both normal and self-test builds keep compiling.

- [ ] **Step 3: Self-test harness header**

Create `main/churn_selftest.h`:
```c
#pragma once
// Runs the M3 logic self-tests against a fake clock + recording apply.
// Returns the number of FAILED checks (0 = all pass).
int churn_selftest_run(void);
```

- [ ] **Step 4: Self-test harness with first roster assertions**

Create `main/churn_selftest.c`:
```c
#include "churn_selftest.h"
#include "roster.h"
#include "esp_log.h"

static const char *TAG = "selftest";
static int s_total, s_fail;
static const char *s_first_fail;

#define ST_CHECK(cond, msg) do { s_total++; if (!(cond)) { \
    s_fail++; if (!s_first_fail) s_first_fail = (msg); \
    ESP_LOGE(TAG, "FAIL: %s", (msg)); } } while (0)

int churn_selftest_run(void)
{
    s_total = 0; s_fail = 0; s_first_fail = NULL;

    // --- roster ---
    roster_init();
    ST_CHECK(roster_count_in_state(ID_IDLE) == CHURN_ROSTER_SIZE,
             "roster_init: all identities IDLE");
    bool macs_ok = true, payload_ok = true;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = roster_at(i);
        if ((id->addr[5] & 0xc0) != 0xc0) macs_ok = false;     // random-static
        if (id->payload_len == 0) payload_ok = false;
    }
    ST_CHECK(macs_ok, "roster: every MAC is random-static (top 2 bits set)");
    ST_CHECK(payload_ok, "roster: every identity has a non-empty payload");

    identity_t *c = roster_promote_candidate(0);
    ST_CHECK(c != NULL, "promote_candidate returns an identity");

    ESP_LOGW(TAG, "SELFTEST: %s (%d/%d)", s_fail ? "FAIL" : "PASS",
             s_total - s_fail, s_total);
    return s_fail;
}
```

- [ ] **Step 5: Self-test mode in `simulacra_main.c`**

At the top of `main/simulacra_main.c` add `#include "churn_selftest.h"` and a flag:
```c
#ifndef CHURN_SELFTEST
#define CHURN_SELFTEST 0
#endif
```
In `simulacra_task()`, branch before `simulacra_run()`:
```c
static void simulacra_task(void *arg)
{
    while (!s_host_synced) vTaskDelay(pdMS_TO_TICKS(10));
#if CHURN_SELFTEST
    int fails = churn_selftest_run();
    for (;;) {  // loop-print so the USB-JTAG reader reliably catches it
        ESP_LOGW("simulacra", "SELFTEST result: %s (fails=%d)",
                 fails ? "FAIL" : "PASS", fails);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    simulacra_run();
#endif
}
```

- [ ] **Step 6: Register sources**

`main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash
)
```

- [ ] **Step 7: Build the self-test (this is the harness landing)**

Set `#define CHURN_SELFTEST 1` in `simulacra_main.c` (it stays 1 through Tasks 2–5; Task 6 sets it back to 0). Then run BUILD-RECIPE (`idf.py build`). Expected: build succeeds.

- [ ] **Step 8: Flash + read self-test**

FLASH-RECIPE, then SELFTEST-READ.
Expected serial: `SELFTEST: PASS (4/4)` and repeating `SELFTEST result: PASS (fails=0)`.

- [ ] **Step 9: Commit**
```powershell
git -C '~\Downloads\simulacra' add main/roster.h main/roster.c main/churn_selftest.h main/churn_selftest.c main/simulacra_main.c main/CMakeLists.txt
git -C '~\Downloads\simulacra' commit -m "feat(m3): persistent identity roster + on-target self-test harness"
```

---

### Task 3: Churn engine — active-set init, dwell expiry, promotion

**Files:**
- Create: `main/churn.h`, `main/churn.c`
- Modify: `main/churn_selftest.c` (add churn lifecycle asserts), `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `roster_promote_candidate`, `identity_t`, `CHURN_HW_INSTANCES`.
- Produces: `typedef void (*churn_apply_fn)(uint8_t, const identity_t*)`; `void churn_set_apply(churn_apply_fn)`; `void churn_init(uint32_t now_ms)`; `void churn_tick(uint32_t now_ms)`; `size_t churn_active_count(void)`; `const identity_t *churn_active_at(size_t)`; tunables `CHURN_ACTIVE_SET`, dwell/cooldown/slice macros.

- [ ] **Step 1: Churn header + tunables**

Create `main/churn.h`:
```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "identity.h"
#include "churn_adv.h"   // CHURN_HW_INSTANCES

#define CHURN_ACTIVE_SET        8
#define CHURN_TICK_MS           250
#define CHURN_SLICE_MS          1000
#define CHURN_DWELL_MIN_MS      180000u    // 3 min
#define CHURN_DWELL_MAX_MS      600000u    // 10 min
#define CHURN_COOLDOWN_MIN_MS   1800000u   // 30 min
#define CHURN_COOLDOWN_MAX_MS   3600000u   // 60 min

typedef void (*churn_apply_fn)(uint8_t instance, const identity_t *id);

void   churn_set_apply(churn_apply_fn fn);
void   churn_init(uint32_t now_ms);
void   churn_tick(uint32_t now_ms);
size_t churn_active_count(void);                 // non-NULL active slots
const identity_t *churn_active_at(size_t slot);  // may be NULL
```

- [ ] **Step 2: Write failing self-test for active-set init + dwell expiry**

Append to `churn_selftest.c` (`#include "churn.h"`), and call these from `churn_selftest_run()` after the roster block:
```c
static void noop_apply(uint8_t instance, const identity_t *id) { (void)instance; (void)id; }

static void test_churn_lifecycle(void)
{
    roster_init();
    churn_set_apply(noop_apply);
    churn_init(0);
    ST_CHECK(churn_active_count() == CHURN_ACTIVE_SET, "init fills the active set");

    // Force slot 0's identity to expire at t=1000, tick, expect replacement.
    const identity_t *before = churn_active_at(0);
    ((identity_t *)before)->active_until_ms = 1000;
    churn_tick(2000);
    const identity_t *after = churn_active_at(0);
    ST_CHECK(after != before, "expired active identity is replaced");
    ST_CHECK(before->state == ID_COOLDOWN, "retired identity enters COOLDOWN");
    ST_CHECK(after && after->state == ID_ACTIVE, "promoted identity is ACTIVE");
}
```
Add `test_churn_lifecycle();` before the final summary log in `churn_selftest_run()`.

- [ ] **Step 3: Build + flash + read — expect FAIL**

`idf.py build`, FLASH-RECIPE, SELFTEST-READ.
Expected: `SELFTEST: FAIL` with a `FAIL: init fills the active set` line (churn.c not implemented).

- [ ] **Step 4: Implement `churn.c`**

Create `main/churn.c`:
```c
#include <string.h>
#include "churn.h"
#include "roster.h"
#include "esp_random.h"

static identity_t   *s_active[CHURN_ACTIVE_SET];
static identity_t   *s_occupant[CHURN_HW_INSTANCES];
static uint32_t      s_phase;
static uint32_t      s_last_slice_ms;
static churn_apply_fn s_apply;

static uint32_t rnd_range(uint32_t lo, uint32_t hi)
{
    return lo + (esp_random() % (hi - lo + 1));
}

void churn_set_apply(churn_apply_fn fn) { s_apply = fn; }

void churn_init(uint32_t now_ms)
{
    s_phase = 0; s_last_slice_ms = now_ms;
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) s_occupant[i] = NULL;
    for (int i = 0; i < CHURN_ACTIVE_SET; i++) {
        identity_t *id = roster_promote_candidate(now_ms);
        s_active[i] = id;
        if (id) {
            id->state = ID_ACTIVE;
            id->active_until_ms = now_ms + rnd_range(1, CHURN_DWELL_MAX_MS); // staggered seed
        }
    }
}

void churn_tick(uint32_t now_ms)
{
    for (int s = 0; s < CHURN_ACTIVE_SET; s++) {
        identity_t *id = s_active[s];
        if (id && now_ms >= id->active_until_ms) {
            id->state = ID_COOLDOWN;
            id->eligible_at_ms = now_ms + rnd_range(CHURN_COOLDOWN_MIN_MS, CHURN_COOLDOWN_MAX_MS);
            identity_t *c = roster_promote_candidate(now_ms);
            s_active[s] = c;
            if (c) {
                c->state = ID_ACTIVE;
                c->active_until_ms = now_ms + rnd_range(CHURN_DWELL_MIN_MS, CHURN_DWELL_MAX_MS);
            }
        }
    }
    if (now_ms - s_last_slice_ms >= CHURN_SLICE_MS) {
        s_last_slice_ms = now_ms; s_phase++;
        for (int i = 0; i < CHURN_HW_INSTANCES; i++) {
            int idx;
            if (CHURN_ACTIVE_SET <= CHURN_HW_INSTANCES) {
                if (i >= CHURN_ACTIVE_SET) continue;     // fewer identities than radios
                idx = i;                                 // static mapping, 100% duty
            } else {
                idx = (s_phase * CHURN_HW_INSTANCES + i) % CHURN_ACTIVE_SET;
            }
            identity_t *target = s_active[idx];
            if (target && target != s_occupant[i]) {
                s_occupant[i] = target;
                if (s_apply) s_apply((uint8_t)i, target);
            }
        }
    }
}

size_t churn_active_count(void)
{
    size_t n = 0;
    for (int i = 0; i < CHURN_ACTIVE_SET; i++) if (s_active[i]) n++;
    return n;
}

const identity_t *churn_active_at(size_t slot)
{
    return (slot < CHURN_ACTIVE_SET) ? s_active[slot] : NULL;
}
```

- [ ] **Step 5: Register `churn.c`**

Add `"churn.c"` to SRCS in `main/CMakeLists.txt`.

- [ ] **Step 6: Build + flash + read — expect PASS**

`idf.py build`, FLASH-RECIPE, SELFTEST-READ.
Expected: `SELFTEST: PASS` with the new lifecycle checks counted.

- [ ] **Step 7: Commit**
```powershell
git -C '~\Downloads\simulacra' add main/churn.h main/churn.c main/churn_selftest.c main/CMakeLists.txt
git -C '~\Downloads\simulacra' commit -m "feat(m3): churn engine active-set init + dwell expiry + promotion"
```

---

### Task 4: Cooldown eligibility + no-reappear-within-cooldown invariant

**Files:** Modify `main/churn_selftest.c`.

**Interfaces:** Consumes Task 3 churn API. Produces no new symbols.

- [ ] **Step 1: Write failing self-tests**

Add to `churn_selftest.c`:
```c
static void test_cooldown(void)
{
    roster_init();
    churn_set_apply(noop_apply);
    churn_init(0);

    // Retire slot 0 at t=1000; capture its MAC + cooldown deadline.
    identity_t *retiring = (identity_t *)churn_active_at(0);
    retiring->active_until_ms = 1000;
    uint8_t mac[6]; memcpy(mac, retiring->addr, 6);
    churn_tick(2000);
    ST_CHECK(retiring->state == ID_COOLDOWN, "retired -> COOLDOWN");
    uint32_t elig = retiring->eligible_at_ms;
    ST_CHECK(elig >= 2000 + CHURN_COOLDOWN_MIN_MS, "cooldown >= min");
    ST_CHECK(elig <= 2000 + CHURN_COOLDOWN_MAX_MS, "cooldown <= max");

    // Long simulation: no MAC re-enters ACTIVE before its eligible_at.
    bool violated = false;
    for (uint32_t t = 2000; t < 4000000u; t += 1000) {
        churn_tick(t);
        for (size_t s = 0; s < CHURN_ACTIVE_SET; s++) {
            const identity_t *a = churn_active_at(s);
            if (a && a == retiring && t < elig) violated = true;
        }
    }
    ST_CHECK(!violated, "no MAC reappears within its cooldown window");
}
```
Call `test_cooldown();` in `churn_selftest_run()`.

- [ ] **Step 2: Build + flash + read**

`idf.py build`, FLASH-RECIPE, SELFTEST-READ.
Expected: PASS (the invariant already holds — `roster_promote_candidate` enforces `eligible_at_ms`). If it FAILs, fix `roster_promote_candidate`'s cooldown gate before proceeding.

- [ ] **Step 3: Commit**
```powershell
git -C '~\Downloads\simulacra' add main/churn_selftest.c
git -C '~\Downloads\simulacra' commit -m "test(m3): cooldown gating + no-reappear-within-cooldown invariant"
```

---

### Task 5: Time-slice mapping across the 4 instances

**Files:** Modify `main/churn_selftest.c`.

- [ ] **Step 1: Write failing self-test for slice coverage**

Add a recording apply + test to `churn_selftest.c`:
```c
static const identity_t *s_rec[CHURN_HW_INSTANCES];
static int s_apply_calls;
static void rec_apply(uint8_t instance, const identity_t *id)
{ if (instance < CHURN_HW_INSTANCES) s_rec[instance] = id; s_apply_calls++; }

static void test_timeslice(void)
{
    roster_init();
    memset(s_rec, 0, sizeof(s_rec));
    s_apply_calls = 0;
    churn_set_apply(rec_apply);
    churn_init(0);
    // Freeze dwell so nobody retires mid-test (deterministic coverage).
    for (int s = 0; s < CHURN_ACTIVE_SET; s++) {
        identity_t *a = (identity_t *)churn_active_at(s);
        if (a) a->active_until_ms = 0xFFFFFFFFu;
    }

    // Collect the identities placed on radios across two consecutive slices.
    const identity_t *seen[CHURN_HW_INSTANCES * 2]; int n = 0;
    churn_tick(CHURN_SLICE_MS);            // slice phase 1
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) seen[n++] = s_rec[i];
    churn_tick(2 * CHURN_SLICE_MS);        // slice phase 2
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) seen[n++] = s_rec[i];

    // With ACTIVE_SET=8, HW=4, every active identity should appear within 2 slices.
    int covered = 0;
    for (int s = 0; s < CHURN_ACTIVE_SET; s++) {
        const identity_t *a = churn_active_at(s);
        for (int j = 0; j < n; j++) if (seen[j] == a) { covered++; break; }
    }
    ST_CHECK(covered == CHURN_ACTIVE_SET,
             "every active identity is on-air within 2 slices");
    ST_CHECK(s_apply_calls > 0, "time-slice drives the apply callback");
}
```
Call `test_timeslice();` in `churn_selftest_run()`.

- [ ] **Step 2: Build + flash + read**

`idf.py build`, FLASH-RECIPE, SELFTEST-READ.
Expected: PASS (mapping implemented in Task 3). If `covered` < 8, re-check the `idx` rotation formula.

- [ ] **Step 3: Commit**
```powershell
git -C '~\Downloads\simulacra' add main/churn_selftest.c
git -C '~\Downloads\simulacra' commit -m "test(m3): time-slice coverage across 4 ext-adv instances"
```

---

### Task 6: Wire churn to NimBLE + integration acceptance + docs

Replace the Task 1 smoke driver with the real churn task (production apply, real clock), verify the M3 acceptance on a scanner, document, and turn the self-test flag back off.

**Files:**
- Modify: `main/simulacra_main.c` (run churn task in normal mode)
- Create: `~\AppData\Local\Temp\simulacra_bleak_m3.py` (persistence-tracking scan)
- Modify: `README.md`, `CHANGELOG.md` (if present)

- [ ] **Step 1: Run the churn engine in normal mode**

In `main/simulacra_main.c`, replace the `#if CHURN_SELFTEST … #else simulacra_run(); #endif` normal branch so the `#else` path runs churn:
```c
#else
    churn_set_apply(churn_adv_apply);
    churn_init(esp_timer_get_time() / 1000);
    for (;;) {
        churn_tick(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(CHURN_TICK_MS));   // yields -> no watchdog
    }
#endif
```
Add includes `#include "churn.h"`, `#include "esp_timer.h"`. Ensure `CHURN_SELFTEST` is back to `0`. The old `simulacra_run()` / smoke driver and the now-unused legacy `#else` ext-adv branch may be deleted (keep `make_random_static_addr`/`build_decoy_fields` only if still referenced; they now live in roster.c).

- [ ] **Step 2: Build (normal mode) + flash**

BUILD-RECIPE (plain `idf.py build`, flag off), confirm build succeeds, then FLASH-RECIPE.

- [ ] **Step 3: Self-test gate one more time (regression)**

`idf.py build`, FLASH-RECIPE, SELFTEST-READ → expect `SELFTEST: PASS (all)`. Then rebuild/flash normal mode (Step 2) so the device ships the real engine.

- [ ] **Step 4: M3 acceptance scan**

Create `~\AppData\Local\Temp\simulacra_bleak_m3.py`:
```python
import asyncio, time
from bleak import BleakScanner
DECOY_CIDS = {0x0075,0x00E0,0x009E,0x0087,0x012D,0x0059}
first, last = {}, {}
def cb(d, adv):
    cids = set(adv.manufacturer_data.keys())
    if cids & DECOY_CIDS:
        now = time.time()
        first.setdefault(d.address, now); last[d.address] = now
async def main():
    s = BleakScanner(detection_callback=cb, scanning_mode="active")
    await s.start(); await asyncio.sleep(180); await s.stop()
    durs = sorted((last[a]-first[a]) for a in first)
    print(f"distinct decoy MACs over 3 min : {len(first)}")
    print(f"persisted >60s                 : {sum(1 for x in durs if x>60)}")
    print(f"max persistence (s)            : {durs[-1] if durs else 0:.0f}")
asyncio.run(main())
```
Run it. Expected over 3 minutes: **more distinct decoy MACs than the active set** (turnover happened), several persisting >60 s (stable identities), and — sampling at any instant — about `CHURN_ACTIVE_SET` present. No watchdog (confirm with SELFTEST-READ-style serial read: 0 `task_wdt`).

- [ ] **Step 5: Tune active-set knob to confirm it's a tunable constant**

Set `CHURN_ACTIVE_SET` to 4 in `churn.h`, rebuild normal mode, flash, BLEAK-SCAN: expect ~4 concurrent. Restore to 8, rebuild, flash.

- [ ] **Step 6: Docs**

Update `README.md` (note the C6 runs the v2 synthetic-population engine; tunables table) and `CHANGELOG.md` if present (add an M3 entry under Unreleased).

- [ ] **Step 7: Commit**
```powershell
git -C '~\Downloads\simulacra' add main/simulacra_main.c README.md CHANGELOG.md
git -C '~\Downloads\simulacra' commit -m "feat(m3): drive ext-adv from churn engine; M3 acceptance verified"
```

---

## Acceptance (M3, from the spec)

- Scanner shows a stable handful (~`CHURN_ACTIVE_SET`) of devices persisting minutes each, staggered arrivals/departures, gradual turnover — not one-shot flicker. ✅ Task 6 Step 4.
- No MAC reappears within its cooldown window. ✅ Task 4 (invariant) + Task 6 (observed).
- Active-set size is a tunable constant. ✅ Task 6 Step 5.
- No task-watchdog over sustained run. ✅ Task 6 Step 4 serial check.
