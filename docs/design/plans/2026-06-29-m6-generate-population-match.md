# M6 Generate-from-Model + Population-Match Implementation Plan



**Goal:** Build the roster by sampling the M5 `rf_model_t` (vendor mix, per-vendor intervals, TX
dither) instead of M4's hand-weighted templates, and size the active set to the observed population
(persona-tuned). Falls back to the M4 population when the model is sparse.

**Architecture:** A new `generate.{h,c}` owns model→identity sampling (no NimBLE dep, unit-testable).
`roster_init` loads the NVS model and chooses generate-vs-fallback. `churn` gains a runtime
active-target (capacity raised to 16). `identity` gains `tx_power`; `churn_adv` applies it.
`templates` gains `template_build_vendor_mfg(company_id)`.

**Tech Stack:** ESP-IDF v5.5 (C5), NimBLE, C. Verified by the looped on-target self-test
(`CHURN_SELFTEST=1`) and a hardware acceptance on the C5 (COM12). Default build stays the decoy.

**Spec:** `docs/design/specs/2026-06-29-m6-generate-population-match-design.md`

## Global Constraints

- **Refined Law 3:** observed Apple `0x004C` → iBeacon (subtype `0x02`); never `0x07`/`0x0F`/`0x12`.
  The self-test `has_apple_popup_subtype()` guard runs over the generated roster.
- **Population-match:** `active_target = clamp(round(pop_ewma*factor), floor, ceiling)`. Ward
  `1.5/6/16`, Shade `1.1/4/8`. Persona defaults from target (`CONFIG_IDF_TARGET_ESP32C5`→Ward).
- **Non-connectable**; aggregates-only (M5); no verbatim replay; payloads ≤31 B.
- Files < 500 lines. Default build = decoy (M4/M5 engine untouched except the additive hooks).

### Reusable recipes (C5 / ESP-IDF 5.5)

**BUILD-RECIPE-C5** (PowerShell):
```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$env:IDF_PATH='~\esp\v5.5\esp-idf'
. ($env:IDF_PATH+'\export.ps1') | Out-Null
Set-Location '~\Downloads\simulacra'
idf.py build
```
**FLASH-RECIPE-C5:** append `idf.py -p COM12 flash`.
**SELFTEST-READ-C5:** `& '~\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe' '~\AppData\Local\Temp\simulacra_read_c5.py'` (resets via RTS/DTR, reads 18 s).
Self-test gated by `#define CHURN_SELFTEST 1` in `simulacra_main.c` (Tasks 1-5; Task 6 sets it to 0).

> **RED on embedded:** new pure functions → RED is a link error (build fails, nothing to flash);
> GREEN = build → flash → SELFTEST-READ shows `PASS (fails=0)`.

---

### Task 1: `template_build_vendor_mfg(company_id)`

**Files:** Modify `main/templates.h`, `main/templates.c`, `main/churn_selftest.c`.

- [ ] **Step 1: Refactor `enc_vendor_mfg` to take a raw company-ID.** In `templates.c`, change the
signature and its one caller:
```c
static void enc_vendor_mfg(uint16_t company_id, struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    mfg[0] = (uint8_t)(company_id & 0xff);
    mfg[1] = (uint8_t)((company_id >> 8) & 0xff);
    mfg[2] = rnd_byte();                       // model/type
    mfg[3] = (uint8_t)(rnd_byte() & 0x0f);     // status flags
    mfg[4] = (uint8_t)(esp_random() % 101);    // battery 0-100
    uint8_t extra = (uint8_t)(1 + (esp_random() % 3));
    for (uint8_t i = 0; i < extra; i++) mfg[5 + i] = rnd_byte();
    f->mfg_data = mfg;
    f->mfg_data_len = (uint8_t)(5 + extra);
}
```
and in `template_build`'s switch: `case FMT_VENDOR_MFG: enc_vendor_mfg(t->company_id, &f, scratch); break;`

- [ ] **Step 2: Add the public builder.** In `templates.h` (after `template_build`):
```c
// Build a generic-but-valid vendor manufacturer-data advertisement for an arbitrary company id
// (for model-driven generation of vendors with no specific template). Returns 0 on success.
int template_build_vendor_mfg(uint16_t company_id, uint8_t out_payload[31], uint8_t *out_len);
```
In `templates.c`:
```c
int template_build_vendor_mfg(uint16_t company_id, uint8_t out_payload[31], uint8_t *out_len)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    static uint8_t scratch[31];
    enc_vendor_mfg(company_id, &f, scratch);
    uint8_t buf[BLE_HS_ADV_MAX_SZ], len = 0;
    if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) { *out_len = 0; return 1; }
    memcpy(out_payload, buf, len);
    *out_len = len;
    return 0;
}
```

- [ ] **Step 3: Self-test** — in `churn_selftest.c`, after `test_rf_model_nvs`:
```c
static void test_vendor_mfg_builder(void)
{
    uint8_t pay[31], len = 0;
    ST_CHECK(template_build_vendor_mfg(0x0040, pay, &len) == 0 && len > 0 && len <= 31,
             "generic vendor-mfg builds for an arbitrary company id");
    bool found = false;                         // company id present in a 0xFF mfg AD, little-endian
    for (int i = 0; i + 1 < len; ) {
        uint8_t adlen = pay[i], adtype = pay[i+1];
        if (adtype == 0xFF && adlen >= 3 && pay[i+2] == 0x40 && pay[i+3] == 0x00) found = true;
        i += adlen + 1;
    }
    ST_CHECK(found, "generic vendor-mfg carries the requested company id");
}
```
Call `test_vendor_mfg_builder();` in `churn_selftest_run()`.

- [ ] **Step 4: RED→GREEN.** `CHURN_SELFTEST 1`. BUILD-RECIPE-C5 (RED = link error before Step 2),
then FLASH + SELFTEST-READ-C5 → `PASS (fails=0)`.

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/templates.h main/templates.c main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m6): template_build_vendor_mfg for arbitrary company ids"
```

---

### Task 2: Per-identity TX power

**Files:** Modify `main/identity.h`, `main/churn_adv.c`.

- [ ] **Step 1: Add the field** — `identity.h`, in the struct (after `adv_itvl_ms`):
```c
    int8_t     tx_power;         // advertising TX power in dBm (per-identity dither; 0 = controller default)
```

- [ ] **Step 2: Apply it** — `churn_adv.c`, replace `p.tx_power = 127;` with:
```c
    p.tx_power = (id->tx_power != 0) ? id->tx_power : 127;   // per-identity dither; 0 -> max/default
```

- [ ] **Step 3: Build only (no new test yet).** BUILD-RECIPE-C5 → succeeds (existing identities
zero-init `tx_power` → behaves as before). No flash needed.

- [ ] **Step 4: Commit**
```bash
git -C "~/Downloads/simulacra" add main/identity.h main/churn_adv.c
git -C "~/Downloads/simulacra" commit -m "feat(m6): per-identity TX power in identity + churn_adv"
```

---

### Task 3: Runtime active-target in churn

**Files:** Modify `main/churn.h`, `main/churn.c`, `main/churn_selftest.c`.

- [ ] **Step 1: Raise capacity + declare the setter** — `churn.h`:
```c
#define CHURN_ACTIVE_SET        16   // MAX active-set capacity (Ward ceiling); runtime target <= this
```
add after `churn_set_apply`:
```c
// Set how many active slots churn fills/manages (1..CHURN_ACTIVE_SET). Call before churn_init.
// Defaults to CHURN_ACTIVE_SET. This is the population-match knob (M6).
void   churn_set_active_target(uint8_t n);
```

- [ ] **Step 2: Honor the runtime target** — `churn.c`. Add `static uint8_t s_active_target = CHURN_ACTIVE_SET;`
and the setter:
```c
void churn_set_active_target(uint8_t n)
{
    if (n < 1) n = 1;
    if (n > CHURN_ACTIVE_SET) n = CHURN_ACTIVE_SET;
    s_active_target = n;
}
```
In `churn_init`, fill `s_active_target` slots (clear the rest):
```c
    for (int i = 0; i < CHURN_ACTIVE_SET; i++) s_active[i] = NULL;
    for (int i = 0; i < s_active_target; i++) {
        identity_t *id = roster_promote_candidate(now_ms);
        s_active[i] = id;
        if (id) { id->state = ID_ACTIVE; id->active_until_ms = now_ms + rnd_range(1, CHURN_DWELL_MAX_MS); }
    }
```
In `churn_tick`, change the retirement loop bound and the time-slice mapping to `s_active_target`:
```c
    for (int s = 0; s < s_active_target; s++) {            // was CHURN_ACTIVE_SET
        ...
    }
    ...
        for (int i = 0; i < CHURN_HW_INSTANCES; i++) {
            int idx;
            if (s_active_target <= CHURN_HW_INSTANCES) {   // was CHURN_ACTIVE_SET
                if (i >= s_active_target) continue;
                idx = i;
            } else {
                idx = (s_phase * CHURN_HW_INSTANCES + i) % s_active_target;   // was CHURN_ACTIVE_SET
            }
            ...
        }
```
In `churn_active_count` and `churn_active_at`, bound by `s_active_target`:
```c
size_t churn_active_count(void) { size_t n=0; for (int i=0;i<s_active_target;i++) if (s_active[i]) n++; return n; }
const identity_t *churn_active_at(size_t slot){ return (slot < s_active_target) ? s_active[slot] : NULL; }
```

- [ ] **Step 3: Generalize `test_timeslice`** — in `churn_selftest.c`, the M3 test assumes 2 slices
cover the active set; with capacity 16 it needs `ceil(CHURN_ACTIVE_SET / CHURN_HW_INSTANCES)` slices.
Replace the two-slice collection with a loop:
```c
    int slices = (CHURN_ACTIVE_SET + CHURN_HW_INSTANCES - 1) / CHURN_HW_INSTANCES;
    const identity_t *seen[CHURN_ACTIVE_SET * 2]; int n = 0;
    for (int sl = 1; sl <= slices; sl++) {
        churn_tick((uint32_t)sl * CHURN_SLICE_MS);
        for (int i = 0; i < CHURN_HW_INSTANCES; i++) seen[n++] = s_rec[i];
    }
```
(the coverage assertion `covered == CHURN_ACTIVE_SET` stays; default target = capacity).

- [ ] **Step 4: RED→GREEN.** `CHURN_SELFTEST 1`. BUILD + FLASH + SELFTEST-READ-C5 → `PASS (fails=0)`
(M3 lifecycle/cooldown/time-slice now run at capacity 16; M4/M5 tests unaffected).

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/churn.h main/churn.c main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m6): runtime churn active-target (capacity 16) for population-match"
```

---

### Task 4: `generate.{h,c}` — model sampling + persona sizing

**Files:** Create `main/generate.h`, `main/generate.c`; modify `main/CMakeLists.txt`,
`main/churn_selftest.c`.

- [ ] **Step 1: Create `main/generate.h`**
```c
#pragma once
#include <stdint.h>
#include "identity.h"
#include "rf_model.h"

#define GEN_MIN_OBS 50   // below this total_obs, the model is too sparse -> caller falls back

// Fill `roster[0..n)` by sampling the model: vendor weighted by observed mix, payload from the
// matching template or a generic vendor-mfg, interval from that vendor's histogram, fresh
// random-static MAC, dithered TX. Every identity gets a valid archetype_idx and a non-empty,
// Law-3-clean payload. Returns the number of identities successfully built (== n on success).
size_t  generate_roster(const rf_model_t *m, identity_t *roster, size_t n);

// Population-matched active-set target from pop_ewma, persona-tuned and clamped.
uint8_t generate_active_target(const rf_model_t *m);
```

- [ ] **Step 2: Create `main/generate.c`**
```c
#include <string.h>
#include "generate.h"
#include "templates.h"
#include "churn.h"          // CHURN_ACTIVE_SET
#include "esp_random.h"

// --- persona profile (factor in tenths, to avoid float in the hot path) ---
#if CONFIG_IDF_TARGET_ESP32C5
#define GEN_FACTOR_X10 15   // Ward: 1.5x
#define GEN_FLOOR      6
#define GEN_CEILING    16
#else
#define GEN_FACTOR_X10 11   // Shade: 1.1x
#define GEN_FLOOR      4
#define GEN_CEILING    8
#endif

extern void make_random_static_addr_pub(uint8_t out[6]);  // see roster.c (Task 5 exposes it)

// interval bin [lo,hi) edges in ms; the >2000 bin caps at 3000.
static const uint16_t ITVL_LO[RF_ITVL_BINS] = {   0,  50, 100, 200,  500, 1000, 2000 };
static const uint16_t ITVL_HI[RF_ITVL_BINS] = {  50, 100, 200, 500, 1000, 2000, 3000 };

static uint16_t rnd_range16(uint16_t lo, uint16_t hi){ return (hi<=lo)?lo:(lo+(esp_random()%(hi-lo))); }

// Weighted pick over counts[0..n); returns index, or -1 if all zero.
static int weighted_pick(const uint32_t *counts, size_t n)
{
    uint64_t total = 0; for (size_t i=0;i<n;i++) total += counts[i];
    if (total == 0) return -1;
    uint64_t r = ((uint64_t)esp_random() << 1 | (esp_random() & 1)) % total;
    for (size_t i=0;i<n;i++){ if (r < counts[i]) return (int)i; r -= counts[i]; }
    return (int)n - 1;
}

// Sample an interval (ms) from a vendor slot's histogram; 0 if the slot has no samples.
static uint16_t sample_interval(const uint32_t bins[RF_ITVL_BINS])
{
    int b = weighted_pick(bins, RF_ITVL_BINS);
    if (b < 0) return 0;
    return rnd_range16(ITVL_LO[b], ITVL_HI[b]);
}

// Map a sampled company id -> a built payload + a representative archetype index (always valid).
// 0x004C -> iBeacon; 0xFFFF (no-mfg) -> a beacon/tracker family; a templated company -> its template;
// otherwise a generic vendor-mfg carrying that company id.
static int build_for_vendor(uint16_t company, uint8_t out[31], uint8_t *len, uint8_t *arch_idx)
{
    // beacon/tracker family indices, discovered once from the template table
    for (size_t i = 0; i < templates_count(); i++) {
        const device_template_t *t = template_at(i);
        if (company == 0x004C && t->family == FMT_IBEACON) {
            uint16_t itvl; uint16_t cid;
            if (template_build(t, out, len, &itvl, &cid) == 0) { *arch_idx=(uint8_t)i; return 0; }
        }
    }
    if (company == RF_VENDOR_UNKNOWN) {   // no-mfg observed -> random beacon/tracker template
        // collect candidate beacon/tracker template indices
        for (int tries = 0; tries < 8; tries++) {
            size_t i = esp_random() % templates_count();
            const device_template_t *t = template_at(i);
            if (t->family==FMT_IBEACON || t->family==FMT_EDDYSTONE_UID ||
                t->family==FMT_EDDYSTONE_URL || t->family==FMT_SVC_TRACKER) {
                uint16_t itvl, cid;
                if (template_build(t, out, len, &itvl, &cid)==0){ *arch_idx=(uint8_t)i; return 0; }
            }
        }
    }
    // a templated vendor-mfg company?
    for (size_t i = 0; i < templates_count(); i++) {
        const device_template_t *t = template_at(i);
        if (t->family==FMT_VENDOR_MFG && t->company_id==company) {
            uint16_t itvl, cid;
            if (template_build(t, out, len, &itvl, &cid)==0){ *arch_idx=(uint8_t)i; return 0; }
        }
    }
    // generic vendor-mfg for an arbitrary company; archetype = first vendor-mfg template (valid idx)
    if (template_build_vendor_mfg(company, out, len) == 0) {
        for (size_t i=0;i<templates_count();i++) if (template_at(i)->family==FMT_VENDOR_MFG){*arch_idx=(uint8_t)i;break;}
        return 0;
    }
    return 1;
}

static int8_t dither_tx(void)   // plausible TX spread; not all at max
{
    static const int8_t lv[] = { -12, -9, -6, -3, 0, 3 };   // 0 -> controller default in churn_adv
    return lv[esp_random() % (sizeof(lv)/sizeof(lv[0]))];
}

size_t generate_roster(const rf_model_t *m, identity_t *roster, size_t n)
{
    // build the vendor sampling table: 24 slots + other + no-mfg(0xFFFF)
    uint32_t counts[RF_VENDOR_SLOTS + 2];
    uint16_t ids[RF_VENDOR_SLOTS + 2];
    size_t k = 0;
    for (size_t i=0;i<RF_VENDOR_SLOTS;i++) if (m->vendors[i].count){ counts[k]=m->vendors[i].count; ids[k]=m->vendors[i].company_id; k++; }
    if (m->other_count){ counts[k]=m->other_count; ids[k]=RF_VENDOR_UNKNOWN; k++; }
    size_t built = 0;
    for (size_t r=0;r<n;r++){
        identity_t *id=&roster[r];
        make_random_static_addr_pub(id->addr);
        int vi = (k>0)? weighted_pick(counts,k) : -1;
        uint16_t company = (vi>=0)? ids[vi] : RF_VENDOR_UNKNOWN;
        uint8_t arch=0;
        if (build_for_vendor(company, id->payload, &id->payload_len, &arch)!=0){ id->payload_len=0; }
        id->company_id = company;
        id->archetype_idx = arch;
        // interval: from the sampled vendor's histogram (else a default 100-300 ms)
        uint16_t itvl = 0;
        if (vi>=0 && vi < (int)RF_VENDOR_SLOTS) itvl = sample_interval(m->vendors[vi].itvl_bins);
        id->adv_itvl_ms = itvl ? itvl : (uint16_t)(100 + (esp_random()%200));
        id->tx_power = dither_tx();
        id->state=ID_IDLE; id->active_until_ms=0; id->eligible_at_ms=0;
        if (id->payload_len) built++;
    }
    return built;
}

uint8_t generate_active_target(const rf_model_t *m)
{
    int t = (int)((m->pop_ewma * GEN_FACTOR_X10 + 5) / 10);   // round(pop*factor)
    if (t < GEN_FLOOR) t = GEN_FLOOR;
    if (t > GEN_CEILING) t = GEN_CEILING;
    if (t > CHURN_ACTIVE_SET) t = CHURN_ACTIVE_SET;
    return (uint8_t)t;
}
```

- [ ] **Step 3: Register + expose the MAC helper.** `main/CMakeLists.txt` SRCS: append `"generate.c"`.
In `roster.c`, change `static void make_random_static_addr` to a public `void make_random_static_addr_pub(uint8_t out[6])` (rename + drop `static`), update its internal caller, and declare it in `roster.h`:
```c
void make_random_static_addr_pub(uint8_t out[6]);   // shared with generate.c
```

- [ ] **Step 4: Self-test** — in `churn_selftest.c` add `#include "generate.h"` and:
```c
static void test_generate(void)
{
    rf_model_t m; rf_model_reset(&m);
    // 70% company 0x0040 @ ~150 ms, 30% Samsung 0x0075 @ ~900 ms
    for (int i=0;i<70;i++) rf_model_observe(&m, 0x0040, -55, 0, 150);
    for (int i=0;i<30;i++) rf_model_observe(&m, 0x0075, -65, 0, 900);
    rf_model_end_sweep(&m, 6, 60000, 6);

    static identity_t roster[64];
    size_t built = generate_roster(&m, roster, 64);
    ST_CHECK(built == 64, "every generated identity has a payload");
    int c40=0, c75=0; bool budget=true, mac=true, nopop=true, arch=true;
    for (size_t i=0;i<64;i++){
        if (roster[i].company_id==0x0040) c40++;
        if (roster[i].company_id==0x0075) c75++;
        if (roster[i].payload_len==0 || roster[i].payload_len>31) budget=false;
        if ((roster[i].addr[5]&0xc0)!=0xc0) mac=false;
        if (has_apple_popup_subtype(roster[i].payload, roster[i].payload_len)) nopop=false;
        if (roster[i].archetype_idx >= templates_count()) arch=false;
    }
    ST_CHECK(budget, "generated payloads fit 31 bytes");
    ST_CHECK(mac, "generated MACs are random-static");
    ST_CHECK(nopop, "generated roster: no forbidden Apple subtype");
    ST_CHECK(arch, "generated identities carry a valid archetype index");
    ST_CHECK(c40 > c75, "generated vendor mix tracks the observed mix (0x0040 dominant)");

    uint8_t at = generate_active_target(&m);   // pop=6 -> Ward 9 / Shade 7, clamped
    ST_CHECK(at >= GEN_FLOOR_TEST_MIN && at <= CHURN_ACTIVE_SET, "active target clamped to persona range");

    rf_model_t empty; rf_model_reset(&empty);
    ST_CHECK(empty.total_obs < GEN_MIN_OBS, "empty model is below the generate threshold (caller falls back)");
}
```
(Define `GEN_FLOOR_TEST_MIN` as 4 at the top of the test file — the lower persona floor — so the
assert holds for either persona build.) Call `test_generate();` in `churn_selftest_run()`.

- [ ] **Step 5: RED→GREEN.** `CHURN_SELFTEST 1`. BUILD (RED before generate.c) → GREEN: FLASH +
SELFTEST-READ-C5 → `PASS (fails=0)`.

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/generate.h main/generate.c main/CMakeLists.txt main/roster.c main/roster.h main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m6): generate module (model sampling + persona-tuned active target)"
```

---

### Task 5: Wire generation into roster + churn

**Files:** Modify `main/roster.c` (model path in `roster_init`), `main/simulacra_main.c` (set churn
active-target from the model), `main/churn_selftest.c` (roster-path test).

- [ ] **Step 1: Model path in `roster_init`** — `main/roster.c`. Keep the current template body as a
static helper `roster_fill_from_templates(void)`; rewrite `roster_init`:
```c
#include "rf_model.h"
#include "generate.h"

void roster_init(void)
{
    rf_model_t m;
    if (rf_model_load_nvs(&m) == 0 && m.total_obs >= GEN_MIN_OBS) {
        generate_roster(&m, s_roster, CHURN_ROSTER_SIZE);
    } else {
        roster_fill_from_templates();          // M4 fallback (fresh / never-observed device)
    }
    s_cursor = 0;
}
```

- [ ] **Step 2: Size the active set from the model** — `main/simulacra_main.c`, in the normal-mode
branch, before `churn_init`:
```c
    roster_init();
    {
        rf_model_t m;
        if (rf_model_load_nvs(&m) == 0 && m.total_obs >= GEN_MIN_OBS)
            churn_set_active_target(generate_active_target(&m));
    }
    churn_set_apply(churn_adv_apply);
    churn_init((uint32_t)(esp_timer_get_time() / 1000));
```
Add `#include "rf_model.h"` and `#include "generate.h"` to `simulacra_main.c`.

- [ ] **Step 3: Roster-path self-test** — in `churn_selftest.c`, extend coverage: build a model, save
to NVS, `roster_init()`, assert the roster reflects generation (valid archetype idx, Law-3-clean,
non-empty), then erase and confirm fallback still yields a valid roster:
```c
static void test_roster_generate_path(void)
{
    rf_model_t m; rf_model_reset(&m);
    for (int i=0;i<80;i++) rf_model_observe(&m, 0x0075, -60, 0, 900);
    rf_model_end_sweep(&m, 5, 60000, 5);
    rf_model_save_nvs(&m);

    roster_init();
    bool ok=true; for (size_t i=0;i<CHURN_ROSTER_SIZE;i++){
        identity_t *id=roster_at(i);
        if (id->payload_len==0 || id->payload_len>31) ok=false;
        if (has_apple_popup_subtype(id->payload,id->payload_len)) ok=false;
        if (id->archetype_idx>=templates_count()) ok=false;
    }
    ST_CHECK(ok, "roster_init generate path: all payloads valid, Law-3-clean, valid archetype");
}
```
Call it in `churn_selftest_run()`. (Leaves an rf_model in NVS — harmless; observe/erase clears it.)

- [ ] **Step 4: RED→GREEN.** `CHURN_SELFTEST 1`. BUILD + FLASH + SELFTEST-READ-C5 → `PASS (fails=0)`.

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/roster.c main/simulacra_main.c main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m6): drive roster + active-set from the observed model (population-match)"
```

---

### Task 6: Hardware acceptance on the C5 + docs

**Files:** Modify `main/simulacra_main.c` (`CHURN_SELFTEST` back to 0), `main/generate.c` (a serial
dump of the generated mix), `README.md`.

- [ ] **Step 1: Self-test regression gate.** `CHURN_SELFTEST 1`, BUILD + FLASH + SELFTEST-READ-C5 →
`PASS (fails=0)` (all M3-M6 checks).

- [ ] **Step 2: Add a generated-mix dump** for acceptance — in `generate.c`, after building the
roster, optionally log the company-id histogram. Add `void generate_dump_roster(const identity_t*, size_t)`
declared in `generate.h` that `ESP_LOGW`s the top company ids + counts + a sample of intervals/tx, and
call it once from `simulacra_main.c`'s normal-mode branch after `roster_init()` (guarded so it prints
once). Keep it terse.

- [ ] **Step 3: Observe the room, then generate.** Build observe mode (`SIMULACRA_OBSERVE 1`),
FLASH-RECIPE-C5, let it run ~1 min near ambient BLE, confirm the model fills (SELFTEST-READ-C5 shows
`MODEL ...`). Then `SIMULACRA_OBSERVE 0`, `CHURN_SELFTEST 0`, BUILD + FLASH. SELFTEST-READ-C5: the
generated-mix dump should show the **observed** company ids (e.g. `0x0040`/`0x0075`), and the device
advertises a population near `generate_active_target` (Ward 6-16).

- [ ] **Step 4: nRF Connect acceptance.** Scan: the decoys now carry the room's company-IDs (not the
hardcoded M4 set), random-static MACs, non-connectable, varied RSSI (TX dither), no pop-ups. Optional:
the two-environments "shift to match" test.

- [ ] **Step 5: Docs.** `README.md` — add an "M6 / population-match" note under Configuration: the
roster is generated from the NVS model when present (vendor mix + intervals + TX dither), sized to the
observed population per persona (`SIMULACRA_PERSONA`, defaults from target: Ward C5 denser, Shade C6
conservative); falls back to the template population when unobserved. Re-profiling = re-run observe
mode.

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/simulacra_main.c main/generate.c main/generate.h README.md
git -C "~/Downloads/simulacra" commit -m "feat(m6): ship model-driven population; C5 acceptance + docs"
```

---

## Acceptance (M6, from the spec)

- Generated vendor mix matches the observed model; active-set size tracks observed population
  (persona-tuned). ✅ self-test (Task 4) + C5 dump/nRF (Task 6).
- Move between two environments → synthesized mix + active-set shift to match. ✅ Task 6 Step 4 (opt).
- No synthesized identity duplicates a real one (fresh random-static MACs; no verbatim replay). ✅.
- Law 3 holds over generated payloads; default build still the decoy. ✅ self-test guard + Task 6.
