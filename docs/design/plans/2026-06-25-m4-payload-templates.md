# M4 Payload Templates Implementation Plan



**Goal:** Replace the roster's `company_id + random bytes` payloads with structurally-valid,
coherently-bundled device advertisements (iBeacon, Eddystone, vendor earbuds/fitness/sensor, Tile),
each chosen as a joint vendor+interval+payload bundle.

**Architecture:** A new `templates.c/.h` defines a table of `device_template_t` "bundles" and a
per-format encoder behind `template_build()` (render + serialize via NimBLE's `ble_hs_adv_set_fields`,
the existing path). `roster_init()` calls `templates_pick()` + `template_build()` per identity and
freezes the result. Churn / active-set / vessel code is untouched. The on-target self-test
(`churn_selftest.c`) gains structural assertions, including the refined-Law-3 forbidden-subtype guard.

**Tech Stack:** ESP-IDF v5.4.4, NimBLE (`ble_hs_adv_fields` / `ble_hs_adv_set_fields`), C. Verified
by `idf.py build` + the looped on-target self-test + a manual nRF Connect scan. Target: the working
**ESP32-C6** node (was COM11).

**Spec:** `docs/design/specs/2026-06-25-m4-payload-templates-design.md`

## Global Constraints

- **Refined Law 3 (safety):** emit only the iBeacon Apple subtype (`4C 00 02 15`); **never** emit
  Continuity proximity-pairing (`0x07`), nearby-action (`0x0F`), or Find My (`0x12`). Enforced by the
  iBeacon encoder's hardcoded prefix **and** a self-test guard.
- Other laws unchanged: aggregates-only, no verbatim replay, **non-connectable** advertising,
  population-match, no raw capture in shipping builds.
- **Payload budget:** legacy AD is **31 bytes**; `ble_hs_adv_set_fields` enforces it (returns nonzero
  on overflow → treat as a build failure for that identity).
- **No heap churn in the hot path:** rendering uses caller-provided scratch buffers; `TEMPLATES[]` is
  `static const`. Files < 500 lines, one responsibility each.
- **Believability bar (M4):** structurally valid + format-correct + coherently bundled. Byte-exact
  cloning of proprietary payloads is M5/M6 — out of scope.

### Reusable recipes (reuse the M3 recipes verbatim)

This plan reuses **BUILD-RECIPE**, **FLASH-RECIPE**, and **SELFTEST-READ** exactly as defined in
`docs/design/plans/2026-06-24-m3-churn-engine.md` (Python 3.12 ahead of PATH, `. export.ps1`,
the C6 COM port, and `simulacra_read.py`). The self-test is gated by `#define CHURN_SELFTEST 1` in
`simulacra_main.c` (set to 1 for Tasks 1–5; Task 6 sets it back to 0).

> **RED on embedded:** a missing function is a *link* error, not a runtime fail. Task 1 shows RED as
> a build-only link failure. Tasks 2–4 instead get a *runtime* RED: `template_build` returns a
> nonzero/empty result for a family whose encoder isn't written yet, so the new self-test assertion
> fails on-target before the encoder exists.

---

### Task 1: Template scaffolding + the vendor-mfg family

**Files:**
- Create: `main/templates.h`, `main/templates.c`
- Modify: `main/CMakeLists.txt` (add `templates.c`)
- Modify: `main/churn_selftest.c` (add template structural tests)

**Interfaces:**
- Produces: `fmt_family_t`, `device_template_t`, `size_t templates_count(void)`,
  `const device_template_t *template_at(size_t)`, `const device_template_t *templates_pick(void)`,
  `int template_build(const device_template_t *t, uint8_t out[31], uint8_t *out_len, uint16_t *out_itvl_ms, uint16_t *out_company_id)` (returns 0 on success).

- [ ] **Step 1: Create `main/templates.h`**

```c
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FMT_VENDOR_MFG,     // company_id + structured blob (earbuds / fitness / sensor)
    FMT_IBEACON,        // 4C 00 02 15 + UUID + major + minor + tx
    FMT_EDDYSTONE_UID,  // svc-data 0xFEAA frame 0x00
    FMT_EDDYSTONE_URL,  // svc-data 0xFEAA frame 0x10
    FMT_SVC_TRACKER,    // service-data tracker (Tile 0xFEED)
} fmt_family_t;

typedef struct {
    const char  *archetype;    // debug/inspection label
    fmt_family_t family;
    uint16_t     company_id;   // vendor-mfg family (0 otherwise)
    uint16_t     svc_uuid;     // service-data families (0xFEAA / 0xFEED)
    const char  *name;         // friendly name (NULL = nameless)
    uint8_t      name_prob;    // % chance to attach the name (0 = never)
    uint16_t     itvl_min_ms;  // joint interval band
    uint16_t     itvl_max_ms;
    uint8_t      weight;       // mix proportion (relative)
} device_template_t;

size_t                   templates_count(void);
const device_template_t *template_at(size_t i);
const device_template_t *templates_pick(void);   // weighted by .weight

// Render template `t` into a frozen advertisement: serialized AD bytes (<=31), on-air interval,
// and the company id (0 for service-data families). Returns 0 on success, nonzero if the fields
// failed to serialize (e.g. over the 31-byte budget).
int template_build(const device_template_t *t, uint8_t out_payload[31], uint8_t *out_len,
                   uint16_t *out_itvl_ms, uint16_t *out_company_id);
```

- [ ] **Step 2: Create `main/templates.c` with the table + vendor-mfg encoder + build/pick**

```c
#include <string.h>
#include "templates.h"
#include "esp_random.h"
#include "host/ble_hs.h"

// --- the bundle library (M4: hand-written; M6 will learn the real mix) ---
static const device_template_t TEMPLATES[] = {
    // archetype     family            company svc    name           np  imin imax  w
    { "earbuds-sams", FMT_VENDOR_MFG,  0x0075, 0,     "Galaxy Buds", 60, 120, 180, 12 },
    { "earbuds-bose", FMT_VENDOR_MFG,  0x009E, 0,     "Bose QC",     60, 120, 180,  6 },
    { "earbuds-sony", FMT_VENDOR_MFG,  0x012D, 0,     NULL,          40, 120, 180,  4 },
    { "fitness-grmn", FMT_VENDOR_MFG,  0x0087, 0,     "vivosmart",   50, 900,1100, 14 },
    { "sensor-nordic",FMT_VENDOR_MFG,  0x0059, 0,     NULL,           0,1800,2200, 18 },
    // beacon + tracker rows are added in Tasks 2-4.
};

static uint16_t rnd_range(uint16_t lo, uint16_t hi) { return lo + (esp_random() % (hi - lo + 1)); }
static uint8_t  rnd_byte(void) { return (uint8_t)(esp_random() & 0xff); }

size_t templates_count(void) { return sizeof(TEMPLATES) / sizeof(TEMPLATES[0]); }
const device_template_t *template_at(size_t i) { return &TEMPLATES[i]; }

const device_template_t *templates_pick(void)
{
    uint32_t total = 0;
    for (size_t i = 0; i < templates_count(); i++) total += TEMPLATES[i].weight;
    uint32_t r = esp_random() % total;
    for (size_t i = 0; i < templates_count(); i++) {
        if (r < TEMPLATES[i].weight) return &TEMPLATES[i];
        r -= TEMPLATES[i].weight;
    }
    return &TEMPLATES[0];
}

// mfg buffer: company(2) + model + status + battery + 1-3 plausible bytes
static void enc_vendor_mfg(const device_template_t *t, struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    mfg[0] = (uint8_t)(t->company_id & 0xff);
    mfg[1] = (uint8_t)((t->company_id >> 8) & 0xff);
    mfg[2] = rnd_byte();                       // model/type
    mfg[3] = (uint8_t)(rnd_byte() & 0x0f);     // status flags
    mfg[4] = (uint8_t)(esp_random() % 101);    // battery 0-100
    uint8_t extra = (uint8_t)(1 + (esp_random() % 3));
    for (uint8_t i = 0; i < extra; i++) mfg[5 + i] = rnd_byte();
    f->mfg_data = mfg;
    f->mfg_data_len = (uint8_t)(5 + extra);
}

int template_build(const device_template_t *t, uint8_t out_payload[31], uint8_t *out_len,
                   uint16_t *out_itvl_ms, uint16_t *out_company_id)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    static uint8_t scratch[31];  // mfg/svc-data working buffer (single task, not reentrant)
    switch (t->family) {
        case FMT_VENDOR_MFG: enc_vendor_mfg(t, &f, scratch); break;
        default: *out_len = 0; return 1;   // unimplemented families (Tasks 2-4) -> RED
    }

    if (t->name && (esp_random() % 100) < t->name_prob) {
        f.name = (uint8_t *)t->name;
        f.name_len = (uint8_t)strlen(t->name);
        f.name_is_complete = 1;
    }
    f.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    f.tx_pwr_lvl_is_present = 1;

    uint8_t buf[BLE_HS_ADV_MAX_SZ], len = 0;
    if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) { *out_len = 0; return 1; }
    memcpy(out_payload, buf, len);
    *out_len = len;
    *out_itvl_ms = rnd_range(t->itvl_min_ms, t->itvl_max_ms);
    *out_company_id = t->company_id;
    return 0;
}
```

- [ ] **Step 3: Add the failing self-test** — in `main/churn_selftest.c`, add `#include "templates.h"`
and a test, called from `churn_selftest_run()` after the existing checks:

```c
static void test_templates(void)
{
    ST_CHECK(templates_count() >= 5, "template library populated");
    bool all_build = true, all_budget = true, all_itvl = true;
    for (size_t i = 0; i < templates_count(); i++) {
        const device_template_t *t = template_at(i);
        uint8_t pay[31], len = 0; uint16_t itvl = 0, cid = 0;
        int rc = template_build(t, pay, &len, &itvl, &cid);
        if (rc != 0 || len == 0) { all_build = false; continue; }
        if (len > 31) all_budget = false;
        if (itvl < t->itvl_min_ms || itvl > t->itvl_max_ms) all_itvl = false;
    }
    ST_CHECK(all_build, "every template builds a non-empty payload");
    ST_CHECK(all_budget, "every template payload fits 31 bytes");
    ST_CHECK(all_itvl, "every interval is within the template band");
}
```
Add `test_templates();` before the summary log in `churn_selftest_run()`.

- [ ] **Step 4: Register `templates.c`** — `main/CMakeLists.txt`:
```cmake
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c" "churn.c" "templates.c"
```

- [ ] **Step 5: Build (RED first if encoder absent), then GREEN.** Ensure `#define CHURN_SELFTEST 1`
in `simulacra_main.c`. Run BUILD-RECIPE → fix any compile errors → FLASH-RECIPE → SELFTEST-READ.
Expected: `SELFTEST result: PASS (fails=0)` (the 5 vendor rows build; beacon families not yet in table).

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/templates.h main/templates.c main/CMakeLists.txt main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m4): template library scaffolding + vendor-mfg encoder"
```

---

### Task 2: iBeacon encoder + refined-Law-3 guard

**Files:** Modify `main/templates.c` (add row + `enc_ibeacon`), `main/churn_selftest.c` (iBeacon + guard tests).

**Interfaces:** Consumes Task 1 API. Produces no new symbols.

- [ ] **Step 1: Add the failing self-test** — append to `test_templates()` (it now iterates the new
iBeacon row once Step 3 adds it). Add a dedicated structural + safety check:

```c
static const device_template_t *find_family(fmt_family_t fam)
{
    for (size_t i = 0; i < templates_count(); i++)
        if (template_at(i)->family == fam) return template_at(i);
    return NULL;
}

static bool has_apple_popup_subtype(const uint8_t *p, uint8_t len)
{
    for (int i = 0; i + 2 < len; i++)
        if (p[i] == 0x4C && p[i+1] == 0x00 &&
            (p[i+2] == 0x07 || p[i+2] == 0x0F || p[i+2] == 0x12)) return true;
    return false;
}

static void test_ibeacon(void)
{
    const device_template_t *t = find_family(FMT_IBEACON);
    ST_CHECK(t != NULL, "iBeacon template present");
    if (!t) return;
    uint8_t pay[31], len = 0; uint16_t itvl = 0, cid = 0;
    ST_CHECK(template_build(t, pay, &len, &itvl, &cid) == 0 && len > 0, "iBeacon builds");
    // locate the manufacturer-data AD (type 0xFF) and check the iBeacon prefix
    bool prefix_ok = false;
    for (int i = 0; i + 1 < len; ) {
        uint8_t adlen = pay[i]; uint8_t adtype = pay[i+1];
        if (adtype == 0xFF && adlen >= 5 &&
            pay[i+2] == 0x4C && pay[i+3] == 0x00 && pay[i+4] == 0x02 && pay[i+5] == 0x15)
            prefix_ok = true;
        i += adlen + 1;
    }
    ST_CHECK(prefix_ok, "iBeacon payload carries 4C 00 02 15");
    ST_CHECK(!has_apple_popup_subtype(pay, len), "iBeacon: no forbidden Apple subtype");
}
```
Call `test_ibeacon();` in `churn_selftest_run()`.

- [ ] **Step 2: Build/flash/read — expect FAIL** (no iBeacon row yet → `find_family` returns NULL →
`"iBeacon template present"` fails). BUILD-RECIPE, FLASH-RECIPE, SELFTEST-READ → `SELFTEST: FAIL`.

- [ ] **Step 3: Add the iBeacon row + encoder** — in `main/templates.c`, add to `TEMPLATES[]`:
```c
    { "ibeacon",      FMT_IBEACON,     0x004C, 0,     NULL,           0,  90, 110, 16 },
```
and the encoder, plus a `case FMT_IBEACON:` in `template_build`'s switch:
```c
static void enc_ibeacon(struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    mfg[0] = 0x4C; mfg[1] = 0x00;          // Apple company id
    mfg[2] = 0x02; mfg[3] = 0x15;          // iBeacon type, length 21
    for (int i = 0; i < 16; i++) mfg[4 + i]  = rnd_byte();   // proximity UUID
    for (int i = 0; i < 4;  i++) mfg[20 + i] = rnd_byte();   // major + minor
    mfg[24] = 0xC5;                         // measured power, -59 dBm
    f->mfg_data = mfg; f->mfg_data_len = 25;
}
```
```c
        case FMT_IBEACON:    enc_ibeacon(&f, scratch); break;
```

- [ ] **Step 4: Build/flash/read — expect PASS.** BUILD-RECIPE, FLASH-RECIPE, SELFTEST-READ →
`SELFTEST result: PASS (fails=0)`.

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/templates.c main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m4): iBeacon encoder + refined-Law-3 forbidden-subtype guard"
```

---

### Task 3: Eddystone-UID + Eddystone-URL encoders

**Files:** Modify `main/templates.c` (rows + encoders), `main/churn_selftest.c` (Eddystone test).

- [ ] **Step 1: Add the failing self-test** — append:
```c
static bool payload_has_svc_uuid16(const uint8_t *p, uint8_t len, uint16_t uuid)
{
    uint8_t lo = uuid & 0xff, hi = (uuid >> 8) & 0xff;
    for (int i = 0; i + 1 < len; ) {
        uint8_t adlen = p[i], adtype = p[i+1];
        // 0x16 = service data 16-bit; first two data bytes are the UUID (little-endian)
        if (adtype == 0x16 && adlen >= 3 && p[i+2] == lo && p[i+3] == hi) return true;
        i += adlen + 1;
    }
    return false;
}

static void test_eddystone(void)
{
    const device_template_t *u = find_family(FMT_EDDYSTONE_UID);
    const device_template_t *r = find_family(FMT_EDDYSTONE_URL);
    ST_CHECK(u && r, "Eddystone UID+URL templates present");
    uint8_t pay[31], len = 0; uint16_t itvl = 0, cid = 0;
    if (u) {
        ST_CHECK(template_build(u, pay, &len, &itvl, &cid) == 0 && len > 0, "Eddystone-UID builds");
        ST_CHECK(payload_has_svc_uuid16(pay, len, 0xFEAA), "Eddystone-UID svc-data 0xFEAA");
    }
    if (r) {
        ST_CHECK(template_build(r, pay, &len, &itvl, &cid) == 0 && len > 0, "Eddystone-URL builds");
        ST_CHECK(payload_has_svc_uuid16(pay, len, 0xFEAA), "Eddystone-URL svc-data 0xFEAA");
    }
}
```
Call `test_eddystone();` in `churn_selftest_run()`.

- [ ] **Step 2: Build/flash/read — expect FAIL** (no Eddystone rows yet).

- [ ] **Step 3: Add rows + encoders.** In `TEMPLATES[]`:
```c
    { "eddy-uid",     FMT_EDDYSTONE_UID, 0,   0xFEAA, NULL,          0,  90, 110, 10 },
    { "eddy-url",     FMT_EDDYSTONE_URL, 0,   0xFEAA, NULL,          0, 650, 750,  6 },
```
Encoders (svc_data_uuid16 carries the 2-byte UUID first, then the frame). Also set the 16-bit
service-UUID list so scanners recognise Eddystone:
```c
#include "host/ble_uuid.h"
static const ble_uuid16_t EDDY_UUID = BLE_UUID16_INIT(0xFEAA);

static void enc_eddystone_uid(struct ble_hs_adv_fields *f, uint8_t *sd)
{
    sd[0] = 0xAA; sd[1] = 0xFE;             // service UUID (little-endian)
    sd[2] = 0x00;                           // frame type: UID
    sd[3] = 0xC5;                           // ranging tx power
    for (int i = 0; i < 10; i++) sd[4 + i]  = rnd_byte();  // namespace
    for (int i = 0; i < 6;  i++) sd[14 + i] = rnd_byte();  // instance
    sd[20] = 0x00; sd[21] = 0x00;           // reserved (drop these two if 31B is tight)
    f->uuids16 = &EDDY_UUID; f->num_uuids16 = 1; f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = sd; f->svc_data_uuid16_len = 22;
}

// Eddystone-URL: scheme 0x03 = "https://", expansion 0x07 = ".com/"
static void enc_eddystone_url(struct ble_hs_adv_fields *f, uint8_t *sd)
{
    static const char *hosts[] = { "example", "acme", "store", "venue" };
    const char *h = hosts[esp_random() % 4];
    uint8_t n = 0;
    sd[n++] = 0xAA; sd[n++] = 0xFE;         // service UUID
    sd[n++] = 0x10;                         // frame type: URL
    sd[n++] = 0xC5;                         // tx power
    sd[n++] = 0x03;                         // scheme https://
    for (const char *c = h; *c; c++) sd[n++] = (uint8_t)*c;
    sd[n++] = 0x07;                         // .com/
    f->uuids16 = &EDDY_UUID; f->num_uuids16 = 1; f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = sd; f->svc_data_uuid16_len = n;
}
```
Add the switch cases:
```c
        case FMT_EDDYSTONE_UID: enc_eddystone_uid(&f, scratch); break;
        case FMT_EDDYSTONE_URL: enc_eddystone_url(&f, scratch); break;
```
> If the UID payload overflows 31 B, drop the two reserved bytes (`svc_data_uuid16_len = 20`); the
> `"fits 31 bytes"` self-test from Task 1 guards this.

- [ ] **Step 4: Build/flash/read — expect PASS.**

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/templates.c main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m4): Eddystone-UID + Eddystone-URL encoders"
```

---

### Task 4: Tile-style tracker (service-data) encoder

**Files:** Modify `main/templates.c` (row + encoder), `main/decoy_vendors.h` (Tile company id, for reference), `main/churn_selftest.c` (tracker test).

- [ ] **Step 1: Add the failing self-test:**
```c
static void test_tracker(void)
{
    const device_template_t *t = find_family(FMT_SVC_TRACKER);
    ST_CHECK(t != NULL, "tracker template present");
    if (!t) return;
    uint8_t pay[31], len = 0; uint16_t itvl = 0, cid = 0;
    ST_CHECK(template_build(t, pay, &len, &itvl, &cid) == 0 && len > 0, "tracker builds");
    ST_CHECK(payload_has_svc_uuid16(pay, len, 0xFEED), "tracker svc-data 0xFEED");
}
```
Call `test_tracker();` in `churn_selftest_run()`.

- [ ] **Step 2: Build/flash/read — expect FAIL.**

- [ ] **Step 3: Add row + encoder.** `decoy_vendors.h` — add a reference comment near `VENDORS[]`:
`// Tile Inc company id 0x0157; Tile devices also advertise service UUID 0xFEED (see templates.c).`
`TEMPLATES[]`:
```c
    { "tile",         FMT_SVC_TRACKER, 0x0157, 0xFEED, NULL,         0,1000,2000, 14 },
```
Encoder + switch case:
```c
static const ble_uuid16_t TILE_UUID = BLE_UUID16_INIT(0xFEED);
static void enc_tracker(struct ble_hs_adv_fields *f, uint8_t *sd)
{
    sd[0] = 0xED; sd[1] = 0xFE;             // Tile service UUID (little-endian)
    for (int i = 0; i < 10; i++) sd[2 + i] = rnd_byte();   // tracker-id-shaped blob
    f->uuids16 = &TILE_UUID; f->num_uuids16 = 1; f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = sd; f->svc_data_uuid16_len = 12;
}
```
```c
        case FMT_SVC_TRACKER: enc_tracker(&f, scratch); break;
```

- [ ] **Step 4: Build/flash/read — expect PASS** (all 5 families now present; the Task-1
`test_templates` budget/interval checks cover every row).

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/templates.c main/decoy_vendors.h main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m4): Tile-style service-data tracker encoder"
```

---

### Task 5: Wire templates into the roster

**Files:** Modify `main/roster.c` (use templates in `roster_init`; delete `build_fields`/`make_random_static_addr` stays), `main/identity.h` (add `archetype_idx`), `main/churn_selftest.c` (roster-wide structural test).

**Interfaces:** Consumes `templates_pick`, `template_build`, `template_at`. The churn engine is unchanged.

- [ ] **Step 1: Add `archetype_idx` to `identity_t`** — `main/identity.h`, inside the struct:
```c
    uint8_t    archetype_idx;    // index into TEMPLATES[], for inspection/test
```

- [ ] **Step 2: Add the failing roster-wide self-test** — in `churn_selftest.c`, extend the existing
roster block (after `roster_init()`), asserting the new structured payloads + the Law-3 guard across
the *whole* 256-identity roster:
```c
static void test_roster_payloads(void)
{
    roster_init();
    bool macs_ok = true, payload_ok = true, no_popup = true;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = roster_at(i);
        if ((id->addr[5] & 0xc0) != 0xc0) macs_ok = false;
        if (id->payload_len == 0 || id->payload_len > 31) payload_ok = false;
        if (has_apple_popup_subtype(id->payload, id->payload_len)) no_popup = false;
    }
    ST_CHECK(macs_ok, "roster: all MACs random-static");
    ST_CHECK(payload_ok, "roster: all payloads non-empty and <=31B");
    ST_CHECK(no_popup, "roster: no forbidden Apple subtype anywhere");
}
```
Call `test_roster_payloads();` in `churn_selftest_run()`. (`has_apple_popup_subtype` from Task 2 is
reused — ensure it's defined above this function.)

- [ ] **Step 3: Build/flash/read — expect FAIL** (`roster_init` still emits the old random payloads;
but they're non-empty, so this likely PASSES trivially — the real RED is structural: until Step 4,
roster payloads are random vendor blobs, not template-built. To force a meaningful RED, the test
also asserts template provenance:) add to `test_roster_payloads` before the summary:
```c
    bool archetype_ok = true;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++)
        if (roster_at(i)->archetype_idx >= templates_count()) archetype_ok = false;
    ST_CHECK(archetype_ok, "roster: every identity carries a valid template index");
```
This fails until Step 4 sets `archetype_idx`. Build/flash/read → `SELFTEST: FAIL`.

- [ ] **Step 4: Rewrite `roster_init` to use templates** — `main/roster.c`. Replace the body of
`roster_init` and delete `build_fields` (keep `make_random_static_addr`); add `#include "templates.h"`:
```c
void roster_init(void)
{
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = &s_roster[i];
        make_random_static_addr(id->addr);
        const device_template_t *t = templates_pick();
        uint16_t itvl = 0, cid = 0;
        if (template_build(t, id->payload, &id->payload_len, &itvl, &cid) != 0) {
            id->payload_len = 0;            // serialization guard; self-test catches this
        }
        id->company_id = cid;
        id->adv_itvl_ms = itvl;
        // record which TEMPLATES[] row this is (for inspection/test)
        id->archetype_idx = 0;
        for (size_t k = 0; k < templates_count(); k++)
            if (template_at(k) == t) { id->archetype_idx = (uint8_t)k; break; }
        id->state = ID_IDLE; id->active_until_ms = 0; id->eligible_at_ms = 0;
    }
    s_cursor = 0;
}
```
Remove the now-unused `SIMULACRA_NAME_PROB`/`SIMULACRA_MFG_PROB` macros and the `build_fields`
function from `roster.c` (name policy now lives in the templates). Drop the
`decoy_vendors.h`/`services/gap/ble_svc_gap.h` includes if no longer referenced.

- [ ] **Step 5: Build/flash/read — expect PASS.** All self-tests (M3 churn lifecycle/cooldown/
time-slice + M4 templates/iBeacon/Eddystone/tracker/roster) pass. BUILD-RECIPE, FLASH-RECIPE,
SELFTEST-READ → `SELFTEST result: PASS (fails=0)`.

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/roster.c main/identity.h main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m4): drive roster payloads from templates (joint bundles)"
```

---

### Task 6: Normal-mode acceptance + docs

**Files:** Modify `main/simulacra_main.c` (`CHURN_SELFTEST` back to 0), `README.md` (archetype/tunables), `docs/design/specs/...` reference only.

- [ ] **Step 1: Self-test regression gate.** With `#define CHURN_SELFTEST 1`, BUILD-RECIPE,
FLASH-RECIPE, SELFTEST-READ one more time → `SELFTEST result: PASS (fails=0)` (all M3 + M4 checks).

- [ ] **Step 2: Ship normal mode.** Set `#define CHURN_SELFTEST 0` in `simulacra_main.c`. BUILD-RECIPE,
FLASH-RECIPE. The device now advertises the template-built population.

- [ ] **Step 3: Serial health check.** SELFTEST-READ-style read of the C6 port: expect **no**
`churn_adv` `rc=` warnings and no `task_wdt` (silent = all applies succeed).

- [ ] **Step 4: Manual nRF Connect acceptance** (phone, ~3 min scan):
  - iBeacons decode as **iBeacon** (UUID / major / minor / TX power).
  - Eddystone-UID / -URL decode as **Eddystone**.
  - Vendor rows show as named manufacturer data (Samsung / Bose / Sony / Garmin / Nordic).
  - Tile rows show service data for UUID `0xFEED`.
  - **No pairing pop-ups** appear on the phone over the whole scan.

- [ ] **Step 5: Docs.** Update `README.md`: replace the `SIMULACRA_NAME_PROB/MFG_PROB` tunables row
with a short "Archetype library" note (the 7 bundles live in `main/templates.c`; mix via the `weight`
column; intervals via the per-template band). Note the refined Law 3 (iBeacon allowed; Continuity/
Find My forbidden).

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/simulacra_main.c README.md
git -C "~/Downloads/simulacra" commit -m "feat(m4): ship template population; nRF Connect acceptance; docs"
```

---

## Acceptance (M4, from the spec)

- Decoys decode cleanly as their claimed format in nRF Connect; iBeacon/Eddystone render as named
  types. ✅ Task 6 Step 4.
- No vendor/payload/interval mismatch — every decoy is a coherent bundle. ✅ self-test (Tasks 1–5).
- No pairing pop-ups; no forbidden Apple subtype ever emitted. ✅ Law-3 self-test guard + Task 6 Step 4.
- Churn behaviour unchanged from M3. ✅ regression (Task 5/6 self-test).
