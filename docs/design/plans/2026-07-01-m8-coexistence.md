# M8 — BLE + Wi-Fi Coexistence, Live Re-Profiling & Anti-Entourage Implementation Plan



**Goal:** Run the BLE decoy and the Wi-Fi probe injector **concurrently on one ESP32-C5/C6** under ESP-IDF software coexistence, and make the synthetic crowd **adapt to the room in real time** (live re-profiling) — with Shade accelerating churn on a location change so the fake crowd never becomes a signature that follows you (anti-entourage).

**Architecture:** One new coordinator module (`coexist.c`) owns a coarse duty-cycle: it drives the existing BLE churn engine continuously, gates Wi-Fi probe bursts (and Ward's 5 GHz excursions) into scheduled windows, and runs a periodic re-profile slot. The platform's `esp_coex` does the microsecond radio arbitration; our code only does coarse scheduling and reuses the M3–M7 modules wholesale. A pure `drift.c` metric serves both re-profiling and anti-entourage.

**Tech Stack:** ESP-IDF v5.5 (Ward/C5) / v5.4 (Shade/C6), NimBLE ext-adv + observer, `esp_wifi_80211_tx`, ESP-IDF SW coexistence, C. Verified by the looped on-target self-test (pure logic) + serial liveness + nRF Connect / Wi-Fi sniffer.

**Spec:** `docs/design/specs/2026-07-01-m8-coexistence-design.md`

## Global Constraints

- **Refined Law 3 (BLE):** advertising is **non-connectable** only. iBeacon (Apple `0x004C`, subtype `0x02`, len `0x15`) IS allowed. NEVER emit Apple Continuity `0x07` / nearby-action `0x0F` / Find My `0x12`, Microsoft `0x0006`, or Google `0xFE2C`. (Already enforced by templates + self-test guards; M8 adds no new payloads.)
- **Wi-Fi Law 3:** wildcard-SSID (broadcast) probe requests ONLY. Never directed / named-SSID. (Enforced by `probe_build_request` + `test_probe_frame`.)
- **Data discipline:** `rf_model` stays aggregates-only (no addr/name/payload). The observe dedup table is RAM-only, per-boot salted, wiped each sweep. No raw capture in shipping builds.
- **Personas:** behavior differentiates by `#if CONFIG_IDF_TARGET_ESP32C5` (Ward: dense, mains, dual-band 2.4+5 GHz) vs C6 (Shade: lean, battery-conscious, 2.4 GHz only).
- **Toolchain:** Ward/C5 → ESP-IDF **v5.5** (`~\esp\v5.5\esp-idf`, target esp32c5, **COM12**), Python 3.12 forced on PATH. Shade/C6 → ESP-IDF **v5.4** (target esp32c6, **COM11**); XIAO C6 needs `-DSIMULACRA_BOARD_XIAO_C6=1`.
- **Project rule:** keep files **< 500 lines**; typed interfaces; TDD (host self-test first, radio idle).
- **Default build (all `SIMULACRA_*` / `CHURN_SELFTEST` = 0) is the shipped decoy** — after M8 that default is the combined BLE+Wi-Fi coexist decoy.

### Build modes after M8

| Flag (all default 0) | Behavior |
|---|---|
| *(none set)* | **Combined coexist decoy — BLE + Wi-Fi together (new default)** |
| `SIMULACRA_PROBE` | Wi-Fi-only probe injector (dev) |
| `SIMULACRA_SNIFF` | Wi-Fi probe sniffer (verification / M9 seed) |
| `SIMULACRA_OBSERVE` | BLE-only ambient observe (dev) |
| `CHURN_SELFTEST` | On-target host-logic self-test (radio idle) |

### Reusable build/flash/read recipes

> **One-time retarget:** the active build target governs which `sdkconfig.defaults.*` is merged. Switch targets with `Remove-Item build,sdkconfig -Recurse -Force; idf.py set-target <esp32c5|esp32c6>` before the first build for that persona.

**BUILD-RECIPE-C5** (Ward, PowerShell):
```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$env:IDF_PATH='~\esp\v5.5\esp-idf'
. ($env:IDF_PATH+'\export.ps1') | Out-Null
Set-Location '~\Downloads\simulacra'
idf.py build 2>&1 | Select-String -Pattern "error|warning|FAIL|Hash of data|succeeded|Project build complete" | Select-Object -Last 30
```
**FLASH-RECIPE-C5:** append `idf.py -p COM12 flash`.
**READ-C5:** `& '~\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe' '~\AppData\Local\Temp\simulacra_read_c5.py'` (resets + reads; filter the line of interest).

**BUILD-RECIPE-C6** (Shade, PowerShell):
```powershell
$env:IDF_PATH='~\esp\v5.4\esp-idf'   # adjust to the installed v5.4 path
. ($env:IDF_PATH+'\export.ps1') | Out-Null
Set-Location '~\Downloads\simulacra'
idf.py -DSIMULACRA_BOARD_XIAO_C6=1 build 2>&1 | Select-String -Pattern "error|warning|FAIL|succeeded|Project build complete" | Select-Object -Last 30
```
**FLASH-RECIPE-C6:** append `idf.py -DSIMULACRA_BOARD_XIAO_C6=1 -p COM11 flash`.
**READ-C6:** `python '~\AppData\Local\Temp\simulacra_read.py'` (COM11).

Self-test gated by `#define CHURN_SELFTEST 1`; the combined decoy is the default (all flags 0).

---

### Task 1: `drift` metric module + self-test (pure)

Drift score = vendor-mix L1 distance + normalized population delta. Pure function, no radio — serves both re-profiling (Layer 2) and anti-entourage (Layer 3).

**Files:**
- Create: `main/drift.h`, `main/drift.c`
- Modify: `main/CMakeLists.txt`, `main/churn_selftest.c`

**Interfaces:**
- Consumes: `rf_model_t`, `rf_vendor_index`, `RF_VENDOR_SLOTS`, `RF_VENDOR_UNKNOWN` (from `rf_model.h`).
- Produces: `float drift_score(const rf_model_t *prev, const rf_model_t *cur)`, `bool drift_exceeds(float score, float threshold)`.

- [ ] **Step 1: Create `main/drift.h`**
```c
#pragma once
#include <stdbool.h>
#include "rf_model.h"

// Drift between two models, 0..~1 (0 = identical environment). Vendor-mix L1
// distance (normalized to 0..1) blended with the normalized population delta.
// Pure: no radio. Shared by live re-profiling (Layer 2) and anti-entourage (Layer 3).
float drift_score(const rf_model_t *prev, const rf_model_t *cur);
// True when score is strictly above threshold.
bool  drift_exceeds(float score, float threshold);
```

- [ ] **Step 2: Create `main/drift.c`**
```c
#include "drift.h"
#include <math.h>

// Fraction of all observations attributed to company_id in model m (0 if absent).
static float vendor_frac(const rf_model_t *m, uint16_t company_id)
{
    uint64_t total = 0;
    for (int i = 0; i < RF_VENDOR_SLOTS; i++) total += m->vendors[i].count;
    total += m->other_count;
    if (total == 0) return 0.0f;
    if (company_id == RF_VENDOR_UNKNOWN) return (float)m->other_count / (float)total;
    int idx = rf_vendor_index(m, company_id);
    if (idx < 0) return 0.0f;
    return (float)m->vendors[idx].count / (float)total;
}

float drift_score(const rf_model_t *prev, const rf_model_t *cur)
{
    // Vendor-mix L1 distance over the union of company ids (+ the no-mfg "other" bucket).
    float l1 = 0.0f;
    for (int i = 0; i < RF_VENDOR_SLOTS; i++)
        if (prev->vendors[i].count) {
            uint16_t c = prev->vendors[i].company_id;
            l1 += fabsf(vendor_frac(prev, c) - vendor_frac(cur, c));
        }
    for (int i = 0; i < RF_VENDOR_SLOTS; i++)
        if (cur->vendors[i].count) {
            uint16_t c = cur->vendors[i].company_id;
            if (rf_vendor_index(prev, c) < 0)          // not already counted from prev
                l1 += fabsf(vendor_frac(prev, c) - vendor_frac(cur, c));
        }
    l1 += fabsf(vendor_frac(prev, RF_VENDOR_UNKNOWN) - vendor_frac(cur, RF_VENDOR_UNKNOWN));
    float mix = l1 * 0.5f;                              // L1 of two distributions is 0..2 -> 0..1

    // Normalized distinct-device (population) delta, 0..1.
    float a = prev->pop_ewma, b = cur->pop_ewma;
    float denom = (a > b) ? a : b;
    float pop = (denom > 0.0f) ? fabsf(a - b) / denom : 0.0f;

    return 0.7f * mix + 0.3f * pop;                     // mix dominant, population secondary
}

bool drift_exceeds(float score, float threshold) { return score > threshold; }
```

- [ ] **Step 3: Register `drift.c`** — `main/CMakeLists.txt`, append `"drift.c"` to SRCS:
```cmake
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c" "churn.c" "templates.c" "rf_model.c" "observe.c" "generate.c" "probe.c" "sniff.c" "drift.c"
```

- [ ] **Step 4: Write the failing test** — in `main/churn_selftest.c` add `#include "drift.h"` near the other includes, then add:
```c
static void test_drift(void)
{
    rf_model_t a; rf_model_reset(&a);                  // 100% Apple
    for (int i=0;i<100;i++) rf_model_observe(&a, 0x004C, -50, 0, -1);
    rf_model_end_sweep(&a, 5, 60000, 5);
    ST_CHECK(drift_score(&a, &a) < 0.01f, "drift: identical models score ~0");

    rf_model_t b; rf_model_reset(&b);                  // 100% Samsung (disjoint mix)
    for (int i=0;i<100;i++) rf_model_observe(&b, 0x0075, -50, 0, -1);
    rf_model_end_sweep(&b, 5, 60000, 5);
    float far = drift_score(&a, &b);
    ST_CHECK(far > 0.6f, "drift: disjoint vendor mixes score high");
    ST_CHECK(drift_exceeds(far, 0.5f), "drift_exceeds true above threshold");

    rf_model_t c; rf_model_reset(&c);                  // 70/30 partial overlap with a
    for (int i=0;i<70;i++) rf_model_observe(&c, 0x004C, -50, 0, -1);
    for (int i=0;i<30;i++) rf_model_observe(&c, 0x0075, -50, 0, -1);
    rf_model_end_sweep(&c, 5, 60000, 5);
    float mid = drift_score(&a, &c);
    ST_CHECK(mid > 0.01f && mid < far, "drift: partial overlap is monotonic with mix distance");
    ST_CHECK(!drift_exceeds(0.1f, 0.5f), "drift_exceeds false below threshold");
}
```
Call `test_drift();` in `churn_selftest_run()` (after `test_probe_frame();`).

- [ ] **Step 5: Run test to verify it fails** — `#define CHURN_SELFTEST 1` in `simulacra_main.c`. BUILD-RECIPE-C5. Expected: **link error** (`undefined reference to drift_score`) until Step 2/3 are in — i.e. confirm the test references the not-yet-linked symbol. (If Steps 2–3 are already committed, instead temporarily comment the body of `drift_score` to `return 0.0f;` and confirm `test_drift` FAILs.)

- [ ] **Step 6: Run test to verify it passes** — with `drift.c` in place: BUILD-RECIPE-C5 → FLASH-RECIPE-C5 → READ-C5, filter `SELFTEST`. Expected: `SELFTEST: PASS` and `SELFTEST result: PASS (fails=0)`.

- [ ] **Step 7: Commit**
```bash
git -C "~/Downloads/simulacra" add main/drift.h main/drift.c main/CMakeLists.txt main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m8): drift metric (vendor-mix L1 + pop delta) + self-test"
```

---

### Task 2: `churn_set_accel` runtime retirement boost + self-test (pure)

A runtime multiplier (≥ 1.0) that shortens churn dwell so active identities retire faster and fresh ones phase in. Default 1.0 = today's behavior. The coordinator decays it back to 1.0 (Layer 3).

**Files:**
- Modify: `main/churn.h`, `main/churn.c`, `main/churn_selftest.c`

**Interfaces:**
- Produces: `void churn_set_accel(float mult)` — multiplier clamped to ≥ 1.0; applied to the dwell window of every identity promoted by `churn_tick`.

- [ ] **Step 1: Declare the API** — `main/churn.h`, after `churn_set_active_target`:
```c
// M8: runtime retirement-rate boost. mult >= 1.0 shortens the dwell window of newly
// promoted identities (mult=1.0 = default behavior). The coordinator decays it back to 1.0.
void   churn_set_accel(float mult);
```

- [ ] **Step 2: Write the failing test** — in `main/churn_selftest.c`, add:
```c
static void test_accel_rotation(void)
{
    roster_init();
    churn_set_apply(noop_apply);
    churn_set_accel(1.0f);
    churn_init(0);

    // With 3x accel, every freshly-promoted dwell is bounded by ~MAX/3.
    churn_set_accel(3.0f);
    uint32_t accel_max = 0, now = 10000;
    for (int k = 0; k < 20; k++) {
        ((identity_t *)churn_active_at(0))->active_until_ms = now;     // force-retire slot 0
        churn_tick(now);
        const identity_t *p = churn_active_at(0);
        if (p) { uint32_t d = p->active_until_ms - now; if (d > accel_max) accel_max = d; }
        now += 1000;
    }
    ST_CHECK(accel_max <= CHURN_DWELL_MAX_MS / 3 + 1, "accel(3.0) bounds dwell to ~MAX/3");
    ST_CHECK(accel_max >= CHURN_DWELL_MIN_MS / 3,     "accel(3.0) dwell still >= MIN/3");

    // Decay back to 1.0 restores the full dwell range (some sample exceeds MAX/3).
    churn_set_accel(1.0f);
    uint32_t base_max = 0;
    for (int k = 0; k < 20; k++) {
        ((identity_t *)churn_active_at(0))->active_until_ms = now;
        churn_tick(now);
        const identity_t *p = churn_active_at(0);
        if (p) { uint32_t d = p->active_until_ms - now; if (d > base_max) base_max = d; }
        now += 1000;
    }
    ST_CHECK(base_max > CHURN_DWELL_MAX_MS / 3, "accel decays to 1.0 -> full dwell range restored");
}
```
Call `test_accel_rotation();` in `churn_selftest_run()` (after `test_drift();`).

- [ ] **Step 3: Run test to verify it fails** — BUILD-RECIPE-C5 (`CHURN_SELFTEST 1`). Expected: **link error** `undefined reference to churn_set_accel`.

- [ ] **Step 4: Implement in `main/churn.c`** — add the state + accessor + a dwell helper, and use it where `churn_tick` promotes. Near the top statics:
```c
static float          s_accel = 1.0f;             // M8: dwell shortening multiplier (>= 1.0)

void churn_set_accel(float mult) { s_accel = (mult < 1.0f) ? 1.0f : mult; }

static uint32_t dwell_ms(void)
{
    uint32_t d = rnd_range(CHURN_DWELL_MIN_MS, CHURN_DWELL_MAX_MS);
    if (s_accel > 1.0f) d = (uint32_t)((float)d / s_accel);
    return d;
}
```
In `churn_tick`, replace the promote dwell line:
```c
                c->active_until_ms = now_ms + rnd_range(CHURN_DWELL_MIN_MS, CHURN_DWELL_MAX_MS);
```
with:
```c
                c->active_until_ms = now_ms + dwell_ms();
```
(Leave `churn_init`'s staggered-seed `rnd_range(1, CHURN_DWELL_MAX_MS)` unchanged.)

- [ ] **Step 5: Run test to verify it passes** — BUILD-RECIPE-C5 → FLASH-RECIPE-C5 → READ-C5, filter `SELFTEST`. Expected: `PASS (fails=0)`.

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/churn.h main/churn.c main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m8): churn_set_accel runtime dwell boost + self-test"
```

---

### Task 3: Refactor `probe` into a scheduler-callable core (keep `SIMULACRA_PROBE`)

Extract `probe_inject_burst(channel)` (one burst on a chosen channel) + Wi-Fi-init + MAC-pool management so the coordinator can call them. Keep `probe_start()`'s forever-loop as a thin wrapper for the existing dev mode (regression-tested on hardware).

**Files:**
- Modify: `main/probe.h`, `main/probe.c`, `main/simulacra_main.c` (only if includes need re-ordering — usually none)

**Interfaces:**
- Produces: `int probe_wifi_init(void)`, `void probe_pool_init(void)`, `int probe_inject_burst(uint8_t channel)`, `int probe_phone_count(void)`, `size_t probe_channels_24(const uint8_t **out)`, `size_t probe_channels_5g(const uint8_t **out)`.
- Consumes (unchanged): `probe_build_request`, `probe_random_mac`, `PROBE_FRAME_MAX`.

- [ ] **Step 1: Extend `main/probe.h`** — after the existing declarations:
```c
// --- scheduler-callable core (M8 coexistence) ---
// Initialize Wi-Fi STA for raw injection. Tolerates an already-created default event loop
// (NimBLE/coexist may run alongside). Returns 0 on success, else the failing esp_err_t.
int    probe_wifi_init(void);
// Seed the persona-sized fake-phone MAC pool. Call once after probe_wifi_init.
void   probe_pool_init(void);
// Inject one burst on `channel`: set the channel, TX a wildcard probe-req from each fake
// phone, occasionally rotate one MAC. Returns the last esp_wifi_80211_tx rc (0 = ok).
int    probe_inject_burst(uint8_t channel);
// Number of fake phones in the pool (persona-sized).
int    probe_phone_count(void);
// Channel hop sets for the scheduler: fills *out, returns count. 5g returns 0 on 2.4-only personas.
size_t probe_channels_24(const uint8_t **out);
size_t probe_channels_5g(const uint8_t **out);
```

- [ ] **Step 2: Refactor `main/probe.c`'s live-radio section.** Replace everything from the `// --- live radio path ---` comment to end-of-file with:
```c
// --- live radio path ---

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "probe";

#if CONFIG_IDF_TARGET_ESP32C5
#define PROBE_PHONES   8
#define PROBE_USE_5G   1
#else
#define PROBE_PHONES   4
#define PROBE_USE_5G   0
#endif
#define PROBE_MAX_PHONES 12
#define PROBE_BURST_MS   2000
#define PROBE_ROTATE_EVERY 15          // ~1-in-15 bursts rotates one fake phone to a fresh MAC

static const uint8_t CH_24[] = { 1, 6, 11 };
#if PROBE_USE_5G
static const uint8_t CH_5[]  = { 36, 40, 44, 48, 149, 153, 157, 161 };
#endif

static uint8_t s_macs[PROBE_MAX_PHONES][6];
static int     s_n;
static int     s_hop;

int probe_wifi_init(void)
{
    esp_err_t e;
    if ((e = esp_netif_init()) != ESP_OK) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;   // tolerate pre-existing loop
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) return e;
    if ((e = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return e;
    if ((e = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return e;
#if CONFIG_IDF_TARGET_ESP32C5
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);               // 2.4 + 5 GHz
#endif
    if ((e = esp_wifi_start()) != ESP_OK) return e;
    return 0;
}

void probe_pool_init(void)
{
    s_n = PROBE_PHONES; if (s_n > PROBE_MAX_PHONES) s_n = PROBE_MAX_PHONES;
    for (int i = 0; i < s_n; i++) probe_random_mac(s_macs[i]);
}

int probe_phone_count(void) { return s_n; }

size_t probe_channels_24(const uint8_t **out) { *out = CH_24; return sizeof(CH_24)/sizeof(CH_24[0]); }
size_t probe_channels_5g(const uint8_t **out)
{
#if PROBE_USE_5G
    *out = CH_5; return sizeof(CH_5)/sizeof(CH_5[0]);
#else
    *out = NULL; return 0;
#endif
}

int probe_inject_burst(uint8_t channel)
{
    int crc = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    int rc = 0;
    for (int i = 0; i < s_n; i++) {
        uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
        probe_build_request(s_macs[i], channel, f, &n);
        rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, true);
    }
    if ((esp_random() % PROBE_ROTATE_EVERY) == 0)               // retire one fake phone -> fresh MAC
        probe_random_mac(s_macs[esp_random() % s_n]);
    ESP_LOGW(TAG, "burst ch=%u phones=%d set_ch_rc=%d tx_rc=%d", channel, s_n, crc, rc);
    return rc;
}

// Dev mode (SIMULACRA_PROBE): forever-loop wrapper over the scheduler core.
static uint8_t next_channel(void)
{
    s_hop++;
#if PROBE_USE_5G
    if (s_hop & 1) return CH_5[(s_hop / 2) % (int)(sizeof(CH_5) / sizeof(CH_5[0]))];
    return CH_24[(s_hop / 2) % (int)(sizeof(CH_24) / sizeof(CH_24[0]))];
#else
    return CH_24[s_hop % (int)(sizeof(CH_24) / sizeof(CH_24[0]))];
#endif
}

static void probe_task(void *arg)
{
    (void)arg;
    for (;;) {
        probe_inject_burst(next_channel());
        vTaskDelay(pdMS_TO_TICKS(PROBE_BURST_MS));
    }
}

void probe_start(void)
{
    if (probe_wifi_init() != 0) { ESP_LOGE(TAG, "wifi init failed"); return; }
    probe_pool_init();
    xTaskCreate(probe_task, "probe", 4096, NULL, 5, NULL);
    ESP_LOGW(TAG, "probe injector started (phones=%d 5g=%d)", PROBE_PHONES, PROBE_USE_5G);
}
```

- [ ] **Step 3: Self-test still passes (no new test).** BUILD-RECIPE-C5 with `#define CHURN_SELFTEST 1` → FLASH → READ-C5: `PASS (fails=0)` (confirms the refactor didn't break the frame/MAC logic exercised by `test_probe_frame`).

- [ ] **Step 4: Hardware regression — `SIMULACRA_PROBE` still injects.** Set `#define CHURN_SELFTEST 0` and `#define SIMULACRA_PROBE 1` in `simulacra_main.c`. BUILD-RECIPE-C5 → FLASH-RECIPE-C5 → READ-C5: expect `probe injector started ...` then repeating `burst ch=.. phones=8 set_ch_rc=0 tx_rc=0` across changing channels (incl. 5 GHz numbers on the C5). Then set `#define SIMULACRA_PROBE 0` again.

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/probe.h main/probe.c main/simulacra_main.c
git -C "~/Downloads/simulacra" commit -m "refactor(m8): split probe into scheduler-callable core (wifi_init/pool/inject_burst)"
```

---

### Task 4: `coexist` scheduler core (persona + pure decision fn) + SW coexistence config

The testable heart of the coordinator: a persona config struct selected by target, and a pure `coexist_due()` that decides which slots fire at a given tick. Plus the ESP-IDF SW-coexistence Kconfig. `coexist_start()` is declared but its body is built in Task 5.

**Files:**
- Create: `main/coexist.h`, `main/coexist.c`
- Modify: `main/CMakeLists.txt`, `main/churn_selftest.c`, `sdkconfig.defaults.esp32c5`, `sdkconfig.defaults.esp32c6`

**Interfaces:**
- Produces: `void coexist_start(void)`, `const coexist_persona_t *coexist_persona(void)`, `coexist_due_t coexist_due(const coexist_persona_t *p, uint32_t now_ms, uint32_t *last_wifi_ms, uint32_t *last_reprofile_ms)`, and the `coexist_persona_t` / `coexist_due_t` types.

- [ ] **Step 1: Create `main/coexist.h`**
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Layer-1 entry: init Wi-Fi under coexistence (BLE-only fallback on failure) and spawn
// the coordinator task that owns churn_tick + Wi-Fi bursts + re-profile. Call after the
// BLE roster/churn are initialized and the NimBLE host is synced.
void coexist_start(void);

// --- testable scheduler core (pure; no radio) ---
typedef struct {
    uint32_t wifi_period_ms;        // Wi-Fi burst cadence
    uint32_t reprofile_period_ms;   // live re-profile cadence
    bool     use_5g;                // C5/Ward: batch 5 GHz excursions
    float    drift_threshold;       // anti-entourage trigger (Ward effectively off)
} coexist_persona_t;

// The persona for this build target (Ward on C5, Shade on C6).
const coexist_persona_t *coexist_persona(void);

typedef struct { bool fire_wifi; bool fire_reprofile; } coexist_due_t;

// Decide what is due at now_ms given last-fire timestamps; advances each timestamp that
// fires. Pure (no radio); shared by the coordinator task and the self-test.
coexist_due_t coexist_due(const coexist_persona_t *p, uint32_t now_ms,
                          uint32_t *last_wifi_ms, uint32_t *last_reprofile_ms);
```

- [ ] **Step 2: Create `main/coexist.c`** (scheduler core only; radio body added in Task 5)
```c
#include "coexist.h"

#if CONFIG_IDF_TARGET_ESP32C5
static const coexist_persona_t s_persona = {       // Ward: dense, mains, dual-band, stationary
    .wifi_period_ms      = 2000,                    // heavier Wi-Fi (~2 s)
    .reprofile_period_ms = 600000,                  // 10 min
    .use_5g              = true,
    .drift_threshold     = 2.0f,                    // unreachable -> anti-entourage off
};
#else
static const coexist_persona_t s_persona = {       // Shade: lean, battery, 2.4-only, portable
    .wifi_period_ms      = 7000,                    // thin Wi-Fi (~6-8 s)
    .reprofile_period_ms = 300000,                  // 5 min
    .use_5g              = false,
    .drift_threshold     = 0.45f,                   // active anti-entourage
};
#endif

const coexist_persona_t *coexist_persona(void) { return &s_persona; }

coexist_due_t coexist_due(const coexist_persona_t *p, uint32_t now_ms,
                          uint32_t *last_wifi_ms, uint32_t *last_reprofile_ms)
{
    coexist_due_t d = { false, false };
    if (now_ms - *last_wifi_ms >= p->wifi_period_ms)            { d.fire_wifi = true;      *last_wifi_ms = now_ms; }
    if (now_ms - *last_reprofile_ms >= p->reprofile_period_ms)  { d.fire_reprofile = true; *last_reprofile_ms = now_ms; }
    return d;
}

// coexist_start() and the coordinator task are implemented in Task 5.
```

- [ ] **Step 3: Register `coexist.c`** — `main/CMakeLists.txt`, append `"coexist.c"` to SRCS:
```cmake
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c" "churn.c" "templates.c" "rf_model.c" "observe.c" "generate.c" "probe.c" "sniff.c" "drift.c" "coexist.c"
```

- [ ] **Step 4: Confirm the SW-coexistence Kconfig symbol against the installed IDF.** Run:
```powershell
Select-String -Path "$env:IDF_PATH\components\esp_coex\Kconfig" -Pattern "SW_COEXIST" | Select-Object -First 5
```
Expected: a `config ESP_COEX_SW_COEXIST_ENABLE` entry. (If the symbol name differs in your IDF, use the name printed here in Step 5.)

- [ ] **Step 5: Enable SW coexistence in both persona defaults.** Append to `sdkconfig.defaults.esp32c5` AND `sdkconfig.defaults.esp32c6`:
```
# M8: software coexistence so the Wi-Fi and BT controllers run concurrently and
# esp_coex arbitrates radio access. Our Wi-Fi use is injection-only (no AP association).
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
```

- [ ] **Step 6: Write the failing test** — in `main/churn_selftest.c` add `#include "coexist.h"`, then:
```c
static void test_scheduler_budget(void)
{
    const coexist_persona_t *p = coexist_persona();
    ST_CHECK(p->wifi_period_ms > 0 && p->reprofile_period_ms > 0, "persona has positive cadences");
    ST_CHECK(p->reprofile_period_ms > p->wifi_period_ms, "re-profile is rarer than Wi-Fi bursts");

    // Drive a 250 ms tick (= CHURN_TICK_MS) over one hour; count fired slots.
    uint32_t lw = 0, lr = 0; int wifi = 0, repro = 0;
    for (uint32_t t = 0; t <= 3600000u; t += 250) {
        coexist_due_t d = coexist_due(p, t, &lw, &lr);
        if (d.fire_wifi)      wifi++;
        if (d.fire_reprofile) repro++;
    }
    int exp_wifi  = (int)(3600000u / p->wifi_period_ms);
    int exp_repro = (int)(3600000u / p->reprofile_period_ms);
    ST_CHECK(wifi  >= exp_wifi  - 1 && wifi  <= exp_wifi  + 1, "Wi-Fi bursts fire at the persona cadence");
    ST_CHECK(repro >= exp_repro - 1 && repro <= exp_repro + 1, "re-profile fires at the persona cadence");

#if CONFIG_IDF_TARGET_ESP32C5
    ST_CHECK(p->use_5g,                  "Ward (C5) schedules 5 GHz excursions");
    ST_CHECK(p->drift_threshold >= 1.0f, "Ward drift threshold effectively off (stationary)");
#else
    ST_CHECK(!p->use_5g,                 "Shade (C6) is 2.4 GHz only");
    ST_CHECK(p->drift_threshold < 1.0f,  "Shade drift threshold active (anti-entourage)");
#endif
}
```
Call `test_scheduler_budget();` in `churn_selftest_run()` (after `test_accel_rotation();`).

- [ ] **Step 7: Run test to verify it fails** — BUILD-RECIPE-C5 (`CHURN_SELFTEST 1`). Expected: **link error** `undefined reference to coexist_persona/coexist_due` until Step 2/3 land. With Step 2/3 in, build passes and the test runs.

- [ ] **Step 8: Run test to verify it passes** — BUILD-RECIPE-C5 → FLASH-RECIPE-C5 → READ-C5, filter `SELFTEST`: `PASS (fails=0)`. (Repeat once for C6 in Task 7; the `#if` arms are validated per-target.)

- [ ] **Step 9: Commit**
```bash
git -C "~/Downloads/simulacra" add main/coexist.h main/coexist.c main/CMakeLists.txt main/churn_selftest.c sdkconfig.defaults.esp32c5 sdkconfig.defaults.esp32c6
git -C "~/Downloads/simulacra" commit -m "feat(m8): coexist scheduler core (persona + due fn) + SW-coexist config + self-test"
```

---

### Task 5: Layer 1 — coordinator task + combined coexist decoy (THE M8 proof)

Implement `coexist_start()` + the coordinator task: churn always-on, Wi-Fi bursts on cadence, C5 5 GHz batched into bounded windows, BLE-only fallback if Wi-Fi init fails. Rewire the `simulacra_main.c` default path to the combined decoy.

**Files:**
- Modify: `main/coexist.c`, `main/simulacra_main.c`

**Interfaces:**
- Consumes: `churn_tick`, `churn_active_count` (churn.h); `probe_wifi_init`, `probe_pool_init`, `probe_inject_burst`, `probe_channels_24`, `probe_channels_5g` (probe.h); `coexist_persona`, `coexist_due` (Task 4).
- Produces: the running combined decoy (no new symbols; `coexist_start` body filled in).

- [ ] **Step 1: Add the radio body to `main/coexist.c`.** Append below the scheduler core:
```c
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "churn.h"
#include "probe.h"

static const char *TAG = "coexist";
#define COEX_TICK_MS   250
#define COEX_5G_EVERY  4               // do a 5 GHz excursion every Nth Wi-Fi burst (keep it sparse)

static bool     s_wifi_ok;
static uint32_t s_wifi_ctr;

#if CONFIG_IDF_TARGET_ESP32C5
// C5 hard exclusion: tuning to 5 GHz means BLE (2.4 GHz) cannot TX. Batch a quick sweep
// across the 5 GHz set, then immediately retune to 2.4 GHz so BLE adv resumes.
static void coexist_5g_excursion(void)
{
    const uint8_t *ch5; size_t n5 = probe_channels_5g(&ch5);
    for (size_t i = 0; i < n5; i++) probe_inject_burst(ch5[i]);
    const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
    if (n24) esp_wifi_set_channel(ch24[0], WIFI_SECOND_CHAN_NONE);   // back to 2.4 GHz
}
#else
static void coexist_5g_excursion(void) {}
#endif

static void coexist_task(void *arg)
{
    (void)arg;
    const coexist_persona_t *p = coexist_persona();
    uint32_t now0 = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_wifi = now0, last_repro = now0;      // don't fire at the instant of boot
    uint32_t hop24 = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        churn_tick(now);                                // BLE ext-adv runs continuously under coex
        coexist_due_t d = coexist_due(p, now, &last_wifi, &last_repro);
        if (d.fire_wifi && s_wifi_ok) {
            const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
            if (p->use_5g && (++s_wifi_ctr % COEX_5G_EVERY == 0)) coexist_5g_excursion();
        }
        if (d.fire_reprofile) {
            // Layer 2 (Task 6) hooks in here. Layer 1: heartbeat only.
            ESP_LOGW(TAG, "reprofile slot (Layer 2 lands in Task 6)");
        }
        vTaskDelay(pdMS_TO_TICKS(COEX_TICK_MS));
    }
}

void coexist_start(void)
{
    int rc = probe_wifi_init();
    s_wifi_ok = (rc == 0);
    if (s_wifi_ok) { probe_pool_init(); ESP_LOGW(TAG, "coexist: wifi up -> BLE + Wi-Fi combined decoy"); }
    else           { ESP_LOGE(TAG, "coexist: wifi init rc=%d -> BLE-only fallback", rc); }
    xTaskCreate(coexist_task, "coexist", 8192, NULL, 5, NULL);
}
```

- [ ] **Step 2: Rewire the `simulacra_main.c` default path.** Add `#include "coexist.h"` with the other includes. Replace the entire `#else ... #endif` default branch of `simulacra_task` (the block that currently does `roster_init()` through the `for(;;){ churn_tick... }` loop) with:
```c
#else
    // Combined coexist decoy (default): set up the BLE population, then hand the tick loop
    // to the coordinator (it owns churn_tick + Wi-Fi bursts + re-profile). roster_init()
    // MUST precede churn_init(): churn pulls identities straight from the roster pool.
    roster_init();
    {
        rf_model_t m;
        if (rf_model_load_nvs(&m) == 0 && m.total_obs >= GEN_MIN_OBS) {
            uint8_t at = generate_active_target(&m);
            churn_set_active_target(at);
            ESP_LOGW(TAG, "population-match: pop=%u active_target=%u",
                     (unsigned)(m.pop_ewma + 0.5f), (unsigned)at);
        }
    }
    churn_set_apply(churn_adv_apply);
    churn_init((uint32_t)(esp_timer_get_time() / 1000));
    coexist_start();
    for (;;) {                                          // this task idles; coexist runs the show
        ESP_LOGW(TAG, "decoy alive active=%u", (unsigned)churn_active_count());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
#endif
```

- [ ] **Step 3: Build + flash the default (C5).** Set all flags 0 (`CHURN_SELFTEST 0`, `SIMULACRA_PROBE 0`, `SIMULACRA_OBSERVE 0`, `SIMULACRA_SNIFF 0`). BUILD-RECIPE-C5 → FLASH-RECIPE-C5. READ-C5 and confirm, in one run: `coexist: wifi up -> BLE + Wi-Fi combined decoy`, repeating `decoy alive active=..`, and repeating `burst ch=.. tx_rc=0` (incl. 5 GHz channel numbers periodically). No `Task watchdog` / `Guru Meditation` / reset loop.

- [ ] **Step 4: THE M8 coexistence proof (user, sustained run).** With the default build flashed on the C5: (a) nRF Connect (or any scanner) sees several random-static (top-bits `≥ 0xC0`) **non-connectable** decoys advertising; **AND** (b) a second device in `SIMULACRA_SNIFF` mode (the XIAO C6) — or Wireshark monitor mode — sees randomized-MAC **wildcard** probe requests; **simultaneously**, over a multi-minute run, with no watchdog/crash. Record the result in the commit body.

- [ ] **Step 5: Fallback sanity (optional but recommended).** Temporarily force `probe_wifi_init` failure path is not easily induced; instead confirm via code review that `s_wifi_ok == false` still spawns `coexist_task` and `churn_tick` runs (BLE-only) — the device must never brick when Wi-Fi init fails.

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/coexist.c main/simulacra_main.c
git -C "~/Downloads/simulacra" commit -m "feat(m8): Layer 1 coexistence coordinator + combined decoy default (BLE+Wi-Fi)" -m "Coexistence proof: scanner sees decoys + sniffer sees probes simultaneously, no watchdog."
```

---

### Task 6: Layer 2 — live re-profiling (observe while advertising → reshape)

On the re-profile cadence: run a bounded `observe_window` while advertising continues, fold into the model, compute drift, then reshape the population gradually (regenerate only IDLE roster entries + resize the active target). No reflash, no hard swap.

**Files:**
- Modify: `main/observe.h`, `main/observe.c`, `main/roster.h`, `main/roster.c`, `main/coexist.c`

**Interfaces:**
- Produces: `void observe_reprofile_init(uint32_t boot_salt)`, `void observe_window(uint32_t duration_ms)`, `const rf_model_t *observe_model(void)` (observe.h); `void roster_reseed_idle(const rf_model_t *m)` (roster.h).
- Consumes: `drift_score` (drift.h); `generate_active_target` (generate.h); `churn_set_active_target` (churn.h).

- [ ] **Step 1: Declare the re-profile API in `main/observe.h`** — after the live-radio section:
```c
// --- live re-profiling (M8): bounded observe window while advertising continues ---
// Load the persistent model once (call before the first observe_window).
void              observe_reprofile_init(uint32_t boot_salt);
// Run ONE bounded scan window (blocks the caller for duration_ms), ingesting reports while
// ext-adv keeps running, then close the sweep and fold into the model.
void              observe_window(uint32_t duration_ms);
// The current persistent model (RAM).
const rf_model_t *observe_model(void);
```

- [ ] **Step 2: Implement the windowed path in `main/observe.c`.** Add a window-mode flag and guard the auto-restart / auto-close so they only run in the standalone observe mode. Near the live-path statics add:
```c
static bool s_window_mode;             // true while observe_window() owns the scan
```
In `observe_gap_event`, change the two standalone-only actions to be gated:
```c
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {        // restart only in standalone mode
        if (!s_window_mode) observe_start_scan();
        return 0;
    }
```
and:
```c
    observe_ingest(&s_model, d->addr.val, now, company, d->rssi, d->legacy_event_type);
    if (!s_window_mode) observe_maybe_close_sweep(now);      // window mode closes explicitly
    return 0;
```
Then append the new functions at end-of-file:
```c
void observe_reprofile_init(uint32_t boot_salt)
{
    observe_reset_ephemeral(boot_salt);                      // sets the per-boot salt
    if (rf_model_load_nvs(&s_model) != 0) rf_model_reset(&s_model);
}

void observe_window(uint32_t duration_ms)
{
    observe_reset_ephemeral(s_salt);                         // fresh dedup table, keep the boot salt
    s_window_mode    = true;
    s_sweep_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    observe_start_scan();                                    // EXT_DISC reports -> observe_gap_event
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ble_gap_disc_cancel();                                   // stop scanning; advertising continues
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    observe_end_sweep(&s_model, now - s_sweep_start_ms);
    s_window_mode = false;
    ESP_LOGW(TAG, "[reprofile sweep %u] pop=%u arr/min=%u obs=%u",
             (unsigned)s_model.sweeps, (unsigned)(s_model.pop_ewma + 0.5f),
             (unsigned)(s_model.arrival_per_min + 0.5f), (unsigned)s_model.total_obs);
}

const rf_model_t *observe_model(void) { return &s_model; }
```
(Add `#include "freertos/FreeRTOS.h"` and `#include "freertos/task.h"` to `observe.c` if not already present, for `vTaskDelay`.)

- [ ] **Step 3: Add gradual reshape to roster.** `main/roster.h` — add `#include "rf_model.h"` and:
```c
// M8 live re-profiling: regenerate ONLY the IDLE identities from a fresh model. ACTIVE and
// COOLDOWN identities keep their MAC/payload, so the visible crowd turns over gradually
// (fresh room-matched identities phase in as churn promotes them; no hard swap).
void        roster_reseed_idle(const rf_model_t *m);
```
`main/roster.c` — append:
```c
void roster_reseed_idle(const rf_model_t *m)
{
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        if (s_roster[i].state != ID_IDLE) continue;
        generate_roster(m, &s_roster[i], 1);   // fills MAC/payload/itvl and sets state = ID_IDLE
    }
}
```

- [ ] **Step 4: Wire re-profiling into the coordinator.** `main/coexist.c` — add `#include "drift.h"`, `#include "observe.h"`, `#include "generate.h"`, `#include "roster.h"`, `#include "rf_model.h"`, `#include "esp_random.h"`, and `#define OBS_REPROFILE_MS 15000`. Add the reprofile function above `coexist_task`:
```c
static void coexist_reprofile(const coexist_persona_t *p)
{
    rf_model_t prev = *observe_model();                     // snapshot pre-update
    observe_window(OBS_REPROFILE_MS);                       // ~15 s scan while advertising
    const rf_model_t *cur = observe_model();
    if (cur->total_obs < GEN_MIN_OBS) {                     // too sparse -> keep current population
        ESP_LOGW(TAG, "reprofile: total_obs=%u < %d -> skip reshape",
                 (unsigned)cur->total_obs, GEN_MIN_OBS);
        return;
    }
    float score = drift_score(&prev, cur);
    roster_reseed_idle(cur);                                // fresh identities into the IDLE pool
    uint8_t at = generate_active_target(cur);
    churn_set_active_target(at);                            // resize to the new population
    ESP_LOGW(TAG, "reprofile: drift=%.3f active_target=%u", score, (unsigned)at);
    coexist_handle_drift(p, score);                         // Layer 3 (Task 7); no-op on Ward
}
```
Add a stub for `coexist_handle_drift` (Task 7 fills it) just above `coexist_reprofile`:
```c
static void coexist_handle_drift(const coexist_persona_t *p, float score) { (void)p; (void)score; }
```
In `coexist_start`, after `probe`/wifi setup, initialize the re-profile model:
```c
    observe_reprofile_init(esp_random());
```
In `coexist_task`, replace the Layer-1 re-profile placeholder with:
```c
        if (d.fire_reprofile) coexist_reprofile(p);
```

- [ ] **Step 5: Build + flash the default (C5), verify re-profiling.** BUILD-RECIPE-C5 (all flags 0) → FLASH-RECIPE-C5. To see it quickly without waiting 10 min, temporarily set the C5 `reprofile_period_ms` to e.g. `60000` (1 min) in `coexist.c`, flash, observe, then restore `600000`. READ-C5: expect, on the cadence, `[reprofile sweep N] pop=.. obs=..` followed by `reprofile: drift=X.XXX active_target=N`. Change the room (power vendor-y devices on/off nearby) and confirm `drift` and `active_target` move across cycles — **no reflash**. Confirm BLE decoys + Wi-Fi bursts continue around the windows (no watchdog).

- [ ] **Step 6: Self-test still passes.** BUILD-RECIPE-C5 with `CHURN_SELFTEST 1` → FLASH → READ-C5: `PASS (fails=0)` (the new radio paths are not exercised by the self-test, but the build must stay green).

- [ ] **Step 7: Commit**
```bash
git -C "~/Downloads/simulacra" add main/observe.h main/observe.c main/roster.h main/roster.c main/coexist.c
git -C "~/Downloads/simulacra" commit -m "feat(m8): Layer 2 live re-profiling (observe_window while advertising -> gradual reshape)"
```

---

### Task 7: Layer 3 — Shade anti-entourage (accelerated gradual rotation + decay)

On Shade (C6), when post-reprofile drift exceeds the persona threshold, boost churn so the carried-over population ages out fast while the already-reshaped room-matched population phases in over ~1–2 min — then decay the boost linearly back to 1.0. Ward's threshold is effectively off.

**Files:**
- Modify: `main/coexist.c`

**Interfaces:**
- Consumes: `drift_exceeds` (drift.h); `churn_set_accel` (churn.h, Task 2); `coexist_persona_t.drift_threshold` (Task 4).

- [ ] **Step 1: Replace the `coexist_handle_drift` stub** in `main/coexist.c`. Add the constants near the top defines:
```c
#define SHADE_DRIFT_ACCEL    3.0f
#define SHADE_ACCEL_DECAY_MS 120000u           // ~2 min linear decay back to 1.0
static uint32_t s_accel_until_ms;              // 0 = not accelerating
```
Replace the stub with:
```c
static void coexist_handle_drift(const coexist_persona_t *p, float score)
{
#if !CONFIG_IDF_TARGET_ESP32C5                  // Shade (C6) only; Ward is stationary
    if (drift_exceeds(score, p->drift_threshold)) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        s_accel_until_ms = now + SHADE_ACCEL_DECAY_MS;
        churn_set_accel(SHADE_DRIFT_ACCEL);
        ESP_LOGW(TAG, "anti-entourage: drift=%.3f > %.2f -> accel=%.1f for %ums",
                 score, p->drift_threshold, SHADE_DRIFT_ACCEL, (unsigned)SHADE_ACCEL_DECAY_MS);
    }
#else
    (void)p; (void)score;
#endif
}
```

- [ ] **Step 2: Add the per-tick decay.** Add above `coexist_task`:
```c
static void coexist_decay_accel(void)
{
    if (s_accel_until_ms == 0) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now >= s_accel_until_ms) {
        churn_set_accel(1.0f);
        s_accel_until_ms = 0;
        ESP_LOGW(TAG, "anti-entourage: accel decayed to 1.0");
        return;
    }
    float frac = (float)(s_accel_until_ms - now) / (float)SHADE_ACCEL_DECAY_MS;   // 1.0 -> 0
    churn_set_accel(1.0f + (SHADE_DRIFT_ACCEL - 1.0f) * frac);                    // linear decay
}
```
In `coexist_task`, call it every tick, right after `churn_tick(now);`:
```c
        churn_tick(now);
        coexist_decay_accel();
```

- [ ] **Step 3: Build + flash on Shade (C6) and verify.** Retarget to C6 (`Remove-Item build,sdkconfig -Recurse -Force; idf.py set-target esp32c6`). BUILD-RECIPE-C6 → FLASH-RECIPE-C6 (all flags 0; `-DSIMULACRA_BOARD_XIAO_C6=1`). To see it without waiting, temporarily set the C6 `reprofile_period_ms` to `60000` in `coexist.c`. READ-C6: trigger an environment change (move the device / change nearby BLE vendors) and confirm the sequence: `[reprofile sweep ..]` → `reprofile: drift=X.XXX ..` with drift over `0.45` → `anti-entourage: drift=.. -> accel=3.0 ..` → (on later ticks) `anti-entourage: accel decayed to 1.0`. Confirm `decoy alive active=..` shows faster MAC turnover during the boost window. Restore `reprofile_period_ms` to `300000`.

- [ ] **Step 4: Self-test on C6.** BUILD-RECIPE-C6 with `CHURN_SELFTEST 1` → FLASH → READ-C6: `PASS (fails=0)` — confirms the Shade `#if` arms of `test_scheduler_budget` (use_5g false, threshold < 1.0) pass on the C6 target.

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/coexist.c
git -C "~/Downloads/simulacra" commit -m "feat(m8): Layer 3 Shade anti-entourage (drift-triggered accel + linear decay)"
```

---

### Task 8: Docs + ship the combined decoy default

**Files:**
- Modify: `main/simulacra_main.c` (confirm all flags 0), `README.md`

- [ ] **Step 1: Confirm shipping defaults.** In `simulacra_main.c`, verify `CHURN_SELFTEST 0`, `SIMULACRA_PROBE 0`, `SIMULACRA_OBSERVE 0`, `SIMULACRA_SNIFF 0`, and the C5/C6 `reprofile_period_ms` are restored to `600000`/`300000`.

- [ ] **Step 2: Update the header comment** at the top of `simulacra_main.c` — change the "Two build modes" note to describe the post-M8 default (combined coexist decoy) and list the dev flags. Keep it under the existing comment block style.

- [ ] **Step 3: Update `README.md`.** Add an "M8 — BLE + Wi-Fi coexistence" section under Configuration documenting: the default build now runs **BLE decoy + Wi-Fi probe injection concurrently** via ESP-IDF SW coexistence; it **live-re-profiles** the room (~10 min Ward / ~5 min Shade) and reshapes the synthetic population with no reflash; Shade adds **anti-entourage** (drift-triggered accelerated churn that decays back to normal). Include the post-M8 build-modes table from this plan's Global Constraints. Note M9 = Wi-Fi observe→model→match.

- [ ] **Step 4: Build + flash both personas one last time to confirm green ship.** BUILD-RECIPE-C5 → FLASH-RECIPE-C5 (default) → READ-C5: `coexist: wifi up ...`, `decoy alive ...`, `burst ... tx_rc=0`. (C6 already confirmed in Task 7.)

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/simulacra_main.c README.md
git -C "~/Downloads/simulacra" commit -m "docs(m8): combined coexist decoy default + M8 README; ship"
```

---

## Acceptance (M8, from the spec)

**Host self-tests** (`CHURN_SELFTEST=1`, all `PASS (fails=0)`):
- `test_drift` — identical → ~0; disjoint mixes → high; monotonic with mix distance. ✅ Task 1.
- `test_scheduler_budget` — persona tick math fires Wi-Fi + re-profile at the right cadence (Ward vs Shade). ✅ Task 4.
- `test_accel_rotation` — `churn_set_accel(3.0)` shortens dwell; decays back to 1.0. ✅ Task 2.

**Hardware (headline proofs):**
1. **Coexistence (both at once)** — default build: scanner sees random-static non-connectable decoys **AND** the sniffer sees randomized probe requests **simultaneously**, sustained, no watchdog/crash. ✅ Task 5 Step 4. *(THE M8 proof.)*
2. **Live re-profiling** — change the room → serial shows model update + population reshape, no reflash. ✅ Task 6 Step 5.
3. **Anti-entourage (Shade)** — environment change → drift over threshold, accelerated rotation, boost decays to 1.0. ✅ Task 7 Step 3.

**Ship:** default build (all flags 0) is the combined coexist decoy; BLE-only fallback if Wi-Fi init fails. ✅ Task 5 (fallback), Task 8.

## Self-review notes (spec coverage)

- Coexistence + duty-cycle (Layer 1) → Tasks 4–5. SW-coexist Kconfig → Task 4 Step 4–5.
- C5 5 GHz bounded-window handling → Task 5 (`coexist_5g_excursion`, sparse via `COEX_5G_EVERY`).
- Live re-profiling (Layer 2): `observe_window` while advertising, EWMA update, drift snapshot, gradual reshape, `total_obs < GEN_MIN_OBS` skip → Task 6.
- Shade anti-entourage (Layer 3): drift trigger, `churn_set_accel(3.0)`, linear decay over `SHADE_ACCEL_DECAY_MS` → Task 7.
- Error handling: Wi-Fi init failure → BLE-only fallback (Task 5); `tx_rc != 0` logged + continue (Task 3 `probe_inject_burst` returns rc, coordinator ignores); sparse-sweep skip (Task 6); `vTaskDelay` every tick (Task 5). ✅
- Module structure & build modes table → Global Constraints + Task 8. ✅
