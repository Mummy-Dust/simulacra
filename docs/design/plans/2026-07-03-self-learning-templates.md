# Self-Learning Templates (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the decoy passively learn the *shape* of real nearby BLE devices and render new decoy identities from those shapes — structure only, never identity.

**Architecture:** A new `learn` module harvests AD-skeletons from the existing observe scan, strips all identifying bytes into a skeleton + randomize-mask, gates them against Law 3, and keeps a capped, NVS-backed store that feeds `generate.c`. A shared `law3` guard is promoted out of the selftest so both it and the learner use one implementation.

**Tech Stack:** C (ESP-IDF), NimBLE AD parsing (`ble_hs_adv_parse_fields`), NVS blob persistence, on-target self-test harness (`churn_selftest.c`, `ST_CHECK`).

## Global Constraints

- **Law 3 (absolute):** a payload must never carry an Apple Continuity pop-up subtype (`0x07` proximity pairing, `0x0F` nearby action) or Find My (`0x12`), nor Microsoft Swift Pair (`0x0006`) nor Google Fast Pair (svc-data `0xFE2C`). iBeacon (`0x02`) is allowed. This applies to learned templates exactly as to hand-authored ones.
- **Learn structure, never identity.** No MAC, name, serial, or unique UUID is ever stored — only the format skeleton with identifying positions masked. Every render differs in value bytes and synthetic name.
- **Payloads:** serialized AD `<= 31` bytes, non-connectable.
- **`learned_template_t` is a flat, pointer-free POD** (Phase 2 ships it over the wire unchanged).
- **Persona split** via `#if CONFIG_IDF_TARGET_ESP32C5` (Ward = C5, Shade = C6). There is **no** `SIMULACRA_PERSONA` flag.
- **NVS** namespace is `"splinter"`.
- **Self-tests run on-target** at boot under `CHURN_SELFTEST=1`; a test that references an unimplemented function fails as a **build/link error** ("undefined reference"), which is the TDD "red" state in this C codebase.
- **Build gate:** the feature is compiled behind `SIMULACRA_LEARN`, default **1** (on for both personas), guarded with `#ifndef SIMULACRA_LEARN / #define SIMULACRA_LEARN 1 / #endif`. The `learn`/`law3` modules themselves always compile (so selftests always run); the flag only gates the live observe hook + generation draw.

### Build / flash / read commands (used by every task's test step)

The self-test harness runs on hardware. Use the C5 (Ward, COM12) as the reference test board. Activate the IDF env once per shell, then build+flash+read with the selftest flag:

```powershell
# one-time per shell:
$env:PATH = "C:\Program Files\Python312;C:\Program Files\Python312\Scripts;$env:PATH"
$env:IDF_PATH = "~\esp\v5.5\esp-idf"; . "$env:IDF_PATH\export.ps1"
Set-Location ~\Downloads\splinter

# build only (link-check a red test): expect "undefined reference" on a not-yet-implemented function
idf.py -B build.c5 -DCHURN_SELFTEST=1 build

# build + flash + read the selftest result:
idf.py -B build.c5 -DCHURN_SELFTEST=1 -p COM12 flash
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM12 --seconds 20 --reset yes --grep "SELFTEST|FAIL:"
```

Expected PASS line: `SELFTEST: PASS (N/N)`. On failure the first failing check prints `FAIL: <msg>` and the summary shows `SELFTEST: FAIL`.

> If the workspace `build.c5` is currently targeted to another chip, first run `idf.py -B build.c5 set-target esp32c5`.

---

### Task 1: Shared Law-3 guard (`law3.{h,c}`)

Promote the `static has_apple_popup_subtype` out of `churn_selftest.c` into a shared module and extend it to cover Swift Pair and Fast Pair, so the learner and the selftest share one guard.

**Files:**
- Create: `main/law3.h`
- Create: `main/law3.c`
- Modify: `main/CMakeLists.txt` (add `law3.c` to SRCS)
- Modify: `main/churn_selftest.c` (delete the local `has_apple_popup_subtype`, `#include "law3.h"`, add a `test_law3()` and call it)

**Interfaces:**
- Produces:
  - `bool has_apple_popup_subtype(const uint8_t *p, uint8_t len);` — unchanged semantics (Apple mfg-data subtype `0x07`/`0x0F`/`0x12`).
  - `bool law3_forbidden(const uint8_t *ad, uint8_t len);` — true if the serialized AD carries **any** forbidden subtype: Apple pop-up (via `has_apple_popup_subtype`), Microsoft Swift Pair (company `0x0006` with beacon subtype), or Google Fast Pair (service-data UUID `0xFE2C`).

- [ ] **Step 1: Write the failing test** — add to `churn_selftest.c` and call `test_law3();` in `churn_selftest_run()` (near `test_ibeacon();`):

```c
#include "law3.h"

static void test_law3(void)
{
    // Apple Continuity nearby-action (0x0F) mfg-data: 4C 00 0F .. -> forbidden
    uint8_t nearby[] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    ST_CHECK(law3_forbidden(nearby, sizeof nearby), "law3: Apple nearby-action forbidden");

    // iBeacon (0x02) is allowed
    uint8_t ibeacon[] = { 0x02,0x01,0x06, 0x1A,0xFF,0x4C,0x00,0x02,0x15,
                          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0, 0,0, 0xC5 };
    ST_CHECK(!law3_forbidden(ibeacon, sizeof ibeacon), "law3: iBeacon allowed");

    // Microsoft Swift Pair: company 0x0006, subtype 0x03 -> forbidden
    uint8_t swift[] = { 0x06,0xFF,0x06,0x00,0x03,0x00 };
    ST_CHECK(law3_forbidden(swift, sizeof swift), "law3: Swift Pair forbidden");

    // Google Fast Pair: service-data UUID 0xFE2C -> forbidden
    uint8_t fastpair[] = { 0x03,0x03,0x2C,0xFE, 0x06,0x16,0x2C,0xFE,0x00,0x00 };
    ST_CHECK(law3_forbidden(fastpair, sizeof fastpair), "law3: Fast Pair forbidden");

    // Plain vendor-mfg (Samsung 0x0075) -> allowed
    uint8_t vendor[] = { 0x06,0xFF,0x75,0x00,0x01,0x02,0x03 };
    ST_CHECK(!law3_forbidden(vendor, sizeof vendor), "law3: plain vendor allowed");
}
```

- [ ] **Step 2: Build to verify it fails** — `idf.py -B build.c5 -DCHURN_SELFTEST=1 build`. Expected: link error `undefined reference to 'law3_forbidden'` (and `has_apple_popup_subtype` now missing if you deleted the static one — delete it in Step 3, not yet).

- [ ] **Step 3: Implement `law3.h`**:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

// True if the payload carries an Apple mfg-data pop-up subtype: 0x07 proximity
// pairing, 0x0F nearby action, or 0x12 Find My. iBeacon (0x02) is NOT flagged.
bool has_apple_popup_subtype(const uint8_t *p, uint8_t len);

// True if the serialized AD (TLV) carries any Law-3 forbidden subtype:
// Apple pop-up (above), Microsoft Swift Pair (company 0x0006), or Google
// Fast Pair (service-data UUID 0xFE2C).
bool law3_forbidden(const uint8_t *ad, uint8_t len);
```

- [ ] **Step 4: Implement `law3.c`** — move the function body from `churn_selftest.c` verbatim, add the AD walker:

```c
#include "law3.h"

// (moved verbatim from churn_selftest.c)
bool has_apple_popup_subtype(const uint8_t *p, uint8_t len)
{
    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t l = p[i];
        if (l == 0 || i + 1 + l > len) break;
        const uint8_t *ad = &p[i + 1];        // ad[0] = type
        if (ad[0] == 0xFF && l >= 4 && ad[1] == 0x4C && ad[2] == 0x00) {
            uint8_t sub = ad[3];
            if (sub == 0x07 || sub == 0x0F || sub == 0x12) return true;
        }
        i += 1 + l;
    }
    return false;
}

bool law3_forbidden(const uint8_t *ad, uint8_t len)
{
    if (has_apple_popup_subtype(ad, len)) return true;
    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t l = ad[i];
        if (l == 0 || i + 1 + l > len) break;
        const uint8_t *e = &ad[i + 1];        // e[0] = type
        uint8_t t = e[0];
        // Microsoft Swift Pair: mfg-data, company 0x0006
        if (t == 0xFF && l >= 3 && e[1] == 0x06 && e[2] == 0x00) return true;
        // Google Fast Pair: 16-bit service data (0x16) for UUID 0xFE2C
        if (t == 0x16 && l >= 3 && e[1] == 0x2C && e[2] == 0xFE) return true;
        i += 1 + l;
    }
    return false;
}
```

> Verify the moved `has_apple_popup_subtype` body matches the original in `churn_selftest.c` before deleting the original. If the original signature differs, keep the original's exact body.

- [ ] **Step 5: Delete the local copy in `churn_selftest.c`** — remove the `static bool has_apple_popup_subtype(...)` definition (the shared one is now used via `#include "law3.h"`). Leave all existing call sites unchanged.

- [ ] **Step 6: Add `law3.c` to `main/CMakeLists.txt` SRCS** — insert `"law3.c"` into the `SRCS` list.

- [ ] **Step 7: Build + flash + read** — `idf.py -B build.c5 -DCHURN_SELFTEST=1 -p COM12 flash` then read serial. Expected: `SELFTEST: PASS (N/N)` with the new `law3:` checks counted and all prior checks still passing.

- [ ] **Step 8: Commit**

```bash
git add main/law3.h main/law3.c main/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(learn): shared Law-3 guard (promote + extend for Swift/Fast Pair)"
```

---

### Task 2: `learned_template_t` + identity-strip gate (`learn.{h,c}`)

Create the module and the strip function: parse a clean AD into a skeleton + randomize-mask, replacing/keeping/rejecting per the policy table.

**Files:**
- Create: `main/learn.h`
- Create: `main/learn.c`
- Modify: `main/CMakeLists.txt` (add `learn.c`)
- Modify: `main/churn_selftest.c` (add `test_learn_strip()`, call it)

**Interfaces:**
- Consumes: `law3_forbidden` (Task 1); `fmt_family_t` from `templates.h`.
- Produces:
  - The `learned_template_t` struct (below).
  - `bool learn_strip(const uint8_t *ad, uint8_t len, uint16_t company, learned_template_t *out);` — returns `false` (capture rejected) if `law3_forbidden`, if parsing fails, or if the stripped skeleton exceeds 31 bytes; otherwise fills `*out` (skeleton, `rand_mask`, `name_off/len`, `company_id`, `svc_uuid`, `family`) and returns `true`. `itvl_*`, `shape_hash`, `reinforce_count`, `last_seen_sweep` are left zero (filled by later tasks).

- [ ] **Step 1: Write the failing test** in `churn_selftest.c` (call `test_learn_strip();`):

```c
#include "learn.h"

static void test_learn_strip(void)
{
    learned_template_t t;

    // Named Samsung earbud: flags + mfg(0075..) + complete-name "Buds Pro"
    uint8_t named[] = { 0x02,0x01,0x06,
                        0x05,0xFF,0x75,0x00,0xAB,0xCD,
                        0x09,0x09,'B','u','d','s',' ','P','r','o' };
    ST_CHECK(learn_strip(named, sizeof named, 0x0075, &t), "strip: named earbud accepted");
    ST_CHECK(t.company_id == 0x0075, "strip: company_id preserved");
    ST_CHECK(t.name_len == 8 && t.name_off > 0, "strip: name region captured");
    // the two mfg blob bytes after company id (AB CD) must be masked
    // (their ad[] offsets are known: flags=3B, then 0x05 0xFF 75 00 AB CD)
    ST_CHECK((t.rand_mask & (1u << 7)) && (t.rand_mask & (1u << 8)),
             "strip: mfg blob bytes masked");
    // company-id bytes (offsets 5,6) must NOT be masked
    ST_CHECK(!(t.rand_mask & (1u << 5)) && !(t.rand_mask & (1u << 6)),
             "strip: company id not masked");

    // Apple nearby-action must be rejected outright
    uint8_t nearby[] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    ST_CHECK(!learn_strip(nearby, sizeof nearby, 0x004C, &t), "strip: forbidden rejected");
}
```

- [ ] **Step 2: Build to verify it fails** — expect `undefined reference to 'learn_strip'`.

- [ ] **Step 3: Implement `learn.h`** (full struct + strip declaration):

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "templates.h"   // fmt_family_t

// A learned archetype: identity-stripped AD skeleton + metadata. Flat POD so it
// is directly serializable (Phase 2 ships it over the wire unchanged).
typedef struct {
    uint8_t  ad[31];          // identity-stripped skeleton (serialized AD bytes)
    uint8_t  ad_len;
    uint8_t  name_off;        // offset of local-name VALUE in ad[] (0 = no name)
    uint8_t  name_len;        // length of local-name value (0 = no name)
    uint32_t rand_mask;       // bit i set => rewrite ad[i] with esp_random() per render
    uint16_t company_id;      // 0 if none
    uint16_t svc_uuid;        // 0 if none
    uint8_t  family;          // fmt_family_t best-effort classification
    uint16_t itvl_min_ms;
    uint16_t itvl_max_ms;
    uint32_t shape_hash;
    uint16_t reinforce_count;
    uint16_t last_seen_sweep;
} learned_template_t;

// Parse a clean AD into a skeleton. false => rejected (forbidden / unparseable /
// over budget). Fills skeleton, rand_mask, name region, company_id, svc_uuid,
// family; leaves interval / shape_hash / counters zero.
bool learn_strip(const uint8_t *ad, uint8_t len, uint16_t company,
                 learned_template_t *out);
```

- [ ] **Step 4: Implement `learn_strip` in `learn.c`** — walk the TLV, apply the policy table:

```c
#include <string.h>
#include "learn.h"
#include "law3.h"

// AD type constants
#define AD_FLAGS        0x01
#define AD_UUID16_INC   0x02
#define AD_UUID16_CMP   0x03
#define AD_NAME_SHORT   0x08
#define AD_NAME_CMP     0x09
#define AD_TXPOWER      0x0A
#define AD_APPEARANCE   0x19
#define AD_SVCDATA16    0x16
#define AD_MFG          0xFF

static void mask_range(uint32_t *m, uint8_t from, uint8_t to)  // [from,to)
{ for (uint8_t i = from; i < to && i < 31; i++) *m |= (1u << i); }

bool learn_strip(const uint8_t *ad, uint8_t len, uint16_t company,
                 learned_template_t *out)
{
    if (!ad || len == 0 || len > 31) return false;
    if (law3_forbidden(ad, len)) return false;

    memset(out, 0, sizeof(*out));
    memcpy(out->ad, ad, len);
    out->ad_len = len;
    out->company_id = company;

    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t l = ad[i];
        if (l == 0 || i + 1 + l > len) return false;     // malformed => reject
        uint8_t type = ad[i + 1];
        uint8_t vfrom = i + 2;                            // first value byte
        uint8_t vto   = i + 1 + l;                        // one past last
        switch (type) {
            case AD_FLAGS: case AD_UUID16_INC: case AD_UUID16_CMP:
            case AD_TXPOWER: case AD_APPEARANCE:
                break;                                    // keep verbatim
            case AD_NAME_SHORT: case AD_NAME_CMP:
                out->name_off = vfrom;                    // replaced at render, not masked
                out->name_len = (uint8_t)(vto - vfrom);
                break;
            case AD_MFG:
                if (vto - vfrom >= 2) {                   // keep company id, mask blob
                    // iBeacon: keep 4C 00 02 15 prefix, mask the rest
                    if (ad[vfrom] == 0x4C && ad[vfrom+1] == 0x00 &&
                        (vto - vfrom) >= 4 && ad[vfrom+2] == 0x02 && ad[vfrom+3] == 0x15)
                        mask_range(&out->rand_mask, vfrom + 4, vto);
                    else
                        mask_range(&out->rand_mask, vfrom + 2, vto);
                } else return false;
                break;
            case AD_SVCDATA16:
                if (vto - vfrom >= 2) {
                    out->svc_uuid = (uint16_t)(ad[vfrom] | (ad[vfrom+1] << 8));
                    mask_range(&out->rand_mask, vfrom + 2, vto);
                } else return false;
                break;
            default:                                      // unknown: keep shape, mask value
                mask_range(&out->rand_mask, vfrom, vto);
                break;
        }
        i += 1 + l;
    }
    // best-effort family (metadata for generation matching)
    if (company == 0x004C) out->family = FMT_IBEACON;
    else if (out->svc_uuid == 0xFEAA) out->family = FMT_EDDYSTONE_UID;
    else if (out->svc_uuid == 0xFEED) out->family = FMT_SVC_TRACKER;
    else out->family = FMT_VENDOR_MFG;
    return true;
}
```

- [ ] **Step 5: Add `learn.c` to `main/CMakeLists.txt` SRCS.**

- [ ] **Step 6: Build + flash + read** — expect `SELFTEST: PASS (N/N)` including the new `strip:` checks.

- [ ] **Step 7: Commit**

```bash
git add main/learn.h main/learn.c main/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(learn): identity-strip AD into skeleton + randomize-mask"
```

---

### Task 3: `shape_hash` + render

Compute a structure-only hash (for dedup) and render a learned template into a fresh, Law-3-clean identity with randomized value bytes and a synthetic name.

**Files:**
- Modify: `main/learn.h` (declare `learn_shape_hash`, `learn_render`)
- Modify: `main/learn.c`
- Modify: `main/churn_selftest.c` (`test_learn_render()`)

**Interfaces:**
- Consumes: `learn_strip` (Task 2); `esp_random()`.
- Produces:
  - `uint32_t learn_shape_hash(const learned_template_t *t);` — FNV-1a over `family`, `company_id`, `svc_uuid`, and the AD-type/length sequence (NOT value bytes).
  - `int learn_render(const learned_template_t *t, uint8_t out[31], uint8_t *out_len, uint16_t *out_itvl_ms);` — copies skeleton, rewrites `rand_mask` bytes with `esp_random()`, overwrites the name region with a synthetic name, samples an interval from `[itvl_min_ms, itvl_max_ms]`. Returns 0 on success (always Law-3-clean, `<=31` B).

- [ ] **Step 1: Write the failing test** (`test_learn_render();`):

```c
static void test_learn_render(void)
{
    uint8_t named[] = { 0x02,0x01,0x06,
                        0x05,0xFF,0x75,0x00,0xAB,0xCD,
                        0x09,0x09,'B','u','d','s',' ','P','r','o' };
    learned_template_t t;
    ST_CHECK(learn_strip(named, sizeof named, 0x0075, &t), "render: strip ok");
    t.itvl_min_ms = 100; t.itvl_max_ms = 200;

    // same structure -> same shape_hash; different company -> different hash
    learned_template_t t2 = t; t2.company_id = 0x009E;
    ST_CHECK(learn_shape_hash(&t) == learn_shape_hash(&t),  "hash: stable");
    ST_CHECK(learn_shape_hash(&t) != learn_shape_hash(&t2), "hash: company changes it");

    uint8_t a[31], b[31]; uint8_t la, lb; uint16_t ia, ib;
    ST_CHECK(learn_render(&t, a, &la, &ia) == 0, "render a ok");
    ST_CHECK(learn_render(&t, b, &lb, &ib) == 0, "render b ok");
    ST_CHECK(la == lb && la <= 31, "render: same length, in budget");
    ST_CHECK(!law3_forbidden(a, la), "render: Law-3 clean");
    ST_CHECK(ia >= 100 && ia <= 200, "render: interval in band");
    // company-id bytes identical across renders, masked blob bytes differ (usually)
    ST_CHECK(a[5] == b[5] && a[6] == b[6], "render: company id stable");
}
```

- [ ] **Step 2: Build to verify it fails** — `undefined reference to 'learn_shape_hash'`.

- [ ] **Step 3: Declare in `learn.h`:**

```c
uint32_t learn_shape_hash(const learned_template_t *t);
int      learn_render(const learned_template_t *t, uint8_t out[31],
                      uint8_t *out_len, uint16_t *out_itvl_ms);
```

- [ ] **Step 4: Implement in `learn.c`** (add `#include "esp_random.h"`):

```c
static const char *SYN_NAMES[] = { "Buds","Watch","Band","Beat","Sensor","Tag","Speaker","Fit" };

uint32_t learn_shape_hash(const learned_template_t *t)
{
    uint32_t h = 2166136261u;
    #define FNV(b) do { h ^= (uint8_t)(b); h *= 16777619u; } while (0)
    FNV(t->family); FNV(t->company_id); FNV(t->company_id >> 8);
    FNV(t->svc_uuid); FNV(t->svc_uuid >> 8);
    for (uint8_t i = 0; i + 1 < t->ad_len; ) {           // type/length sequence only
        uint8_t l = t->ad[i];
        if (l == 0 || i + 1 + l > t->ad_len) break;
        FNV(l); FNV(t->ad[i + 1]);
        i += 1 + l;
    }
    #undef FNV
    return h;
}

int learn_render(const learned_template_t *t, uint8_t out[31],
                 uint8_t *out_len, uint16_t *out_itvl_ms)
{
    memcpy(out, t->ad, t->ad_len);
    for (uint8_t i = 0; i < t->ad_len && i < 31; i++)
        if (t->rand_mask & (1u << i)) out[i] = (uint8_t)(esp_random() & 0xff);
    if (t->name_len) {                                   // synthetic name, fit to length
        const char *nm = SYN_NAMES[esp_random() % (sizeof(SYN_NAMES)/sizeof(SYN_NAMES[0]))];
        uint8_t nl = (uint8_t)strlen(nm);
        for (uint8_t i = 0; i < t->name_len; i++)
            out[t->name_off + i] = (i < nl) ? (uint8_t)nm[i] : (uint8_t)('0' + (esp_random() % 10));
    }
    *out_len = t->ad_len;
    uint16_t lo = t->itvl_min_ms, hi = t->itvl_max_ms;
    if (hi < lo) hi = lo;
    *out_itvl_ms = lo + (hi > lo ? (uint16_t)(esp_random() % (hi - lo + 1)) : 0);
    return 0;
}
```

- [ ] **Step 5: Build + flash + read** — expect `SELFTEST: PASS` with `hash:` and `render:` checks.

- [ ] **Step 6: Commit**

```bash
git add main/learn.h main/learn.c main/churn_selftest.c
git commit -m "feat(learn): shape_hash dedup key + skeleton render with synthetic name"
```

---

### Task 4: Learned store — add / reinforce / dedup / cap / age-out

The bounded RAM store with dedup by `shape_hash`, saturating reinforcement, weakest-first eviction, and sweep-based age-out.

**Files:**
- Modify: `main/learn.h` (store API + tunables)
- Modify: `main/learn.c`
- Modify: `main/churn_selftest.c` (`test_learn_store()`)

**Interfaces:**
- Consumes: `learn_shape_hash` (Task 3).
- Produces:
  - `#define LEARN_CAP` (persona: C5 128 / C6 64), `LEARN_AGEOUT_SWEEPS`.
  - `void learn_reset(void);`
  - `size_t learn_count(void);`
  - `const learned_template_t *learn_at(size_t i);`
  - `bool learn_store_add(const learned_template_t *t, uint16_t sweep_no);` — dedup by `shape_hash`: if present, reinforce (saturating `reinforce_count++`, refresh `last_seen_sweep`, widen interval band) and return true; else insert, evicting the weakest (lowest `reinforce_count`, tie oldest `last_seen_sweep`) if full. Returns false only if the candidate is weaker than every existing entry when full.
  - `void learn_age_out(uint16_t sweep_no);` — evict entries with `sweep_no - last_seen_sweep > LEARN_AGEOUT_SWEEPS`.

- [ ] **Step 1: Write the failing test** (`test_learn_store();`):

```c
static void mk_shape(learned_template_t *t, uint16_t company)
{
    memset(t, 0, sizeof(*t));
    t->ad_len = 6; t->ad[0]=0x05; t->ad[1]=0xFF;
    t->ad[2]=(uint8_t)company; t->ad[3]=(uint8_t)(company>>8); t->ad[4]=1; t->ad[5]=2;
    t->company_id = company; t->family = FMT_VENDOR_MFG;
    t->itvl_min_ms = 100; t->itvl_max_ms = 200;
    t->shape_hash = learn_shape_hash(t);
}

static void test_learn_store(void)
{
    learn_reset();
    ST_CHECK(learn_count() == 0, "store: empty after reset");

    learned_template_t a; mk_shape(&a, 0x0075);
    ST_CHECK(learn_store_add(&a, 1) && learn_count() == 1, "store: first add");

    // same shape again -> reinforce, no duplicate
    ST_CHECK(learn_store_add(&a, 2) && learn_count() == 1, "store: dedup reinforces");
    ST_CHECK(learn_at(0)->reinforce_count >= 1, "store: reinforce_count bumped");

    // distinct shape -> second entry
    learned_template_t b; mk_shape(&b, 0x009E);
    ST_CHECK(learn_store_add(&b, 2) && learn_count() == 2, "store: distinct shape added");

    // age-out drops b (last_seen sweep 2) at a far future sweep
    learn_age_out(2 + LEARN_AGEOUT_SWEEPS + 1);
    ST_CHECK(learn_count() < 2, "store: stale entry aged out");
}
```

- [ ] **Step 2: Build to verify it fails** — `undefined reference to 'learn_reset'`.

- [ ] **Step 3: Add tunables + declarations to `learn.h`:**

```c
#if CONFIG_IDF_TARGET_ESP32C5
#define LEARN_CAP 128
#else
#define LEARN_CAP 64
#endif
#ifndef LEARN_AGEOUT_SWEEPS
#define LEARN_AGEOUT_SWEEPS 720   // ~ hours at a 15 s sweep; tunable
#endif

void   learn_reset(void);
size_t learn_count(void);
const learned_template_t *learn_at(size_t i);
bool   learn_store_add(const learned_template_t *t, uint16_t sweep_no);
void   learn_age_out(uint16_t sweep_no);
```

- [ ] **Step 4: Implement in `learn.c`** (add `#include "sdkconfig.h"` at top for `CONFIG_IDF_TARGET_ESP32C5`):

```c
static learned_template_t s_store[LEARN_CAP];
static size_t             s_count;

void   learn_reset(void) { memset(s_store, 0, sizeof(s_store)); s_count = 0; }
size_t learn_count(void) { return s_count; }
const learned_template_t *learn_at(size_t i) { return (i < s_count) ? &s_store[i] : NULL; }

static int find_shape(uint32_t hash)
{
    for (size_t i = 0; i < s_count; i++) if (s_store[i].shape_hash == hash) return (int)i;
    return -1;
}
static size_t weakest(void)   // lowest reinforce_count, tie -> oldest last_seen
{
    size_t w = 0;
    for (size_t i = 1; i < s_count; i++) {
        if (s_store[i].reinforce_count < s_store[w].reinforce_count ||
            (s_store[i].reinforce_count == s_store[w].reinforce_count &&
             s_store[i].last_seen_sweep < s_store[w].last_seen_sweep)) w = i;
    }
    return w;
}

bool learn_store_add(const learned_template_t *t, uint16_t sweep_no)
{
    int idx = find_shape(t->shape_hash);
    if (idx >= 0) {                                    // reinforce existing
        learned_template_t *e = &s_store[idx];
        if (e->reinforce_count < 0xFFFF) e->reinforce_count++;
        e->last_seen_sweep = sweep_no;
        if (t->itvl_min_ms < e->itvl_min_ms) e->itvl_min_ms = t->itvl_min_ms;
        if (t->itvl_max_ms > e->itvl_max_ms) e->itvl_max_ms = t->itvl_max_ms;
        return true;
    }
    if (s_count < LEARN_CAP) {                          // free slot
        s_store[s_count] = *t;
        s_store[s_count].last_seen_sweep = sweep_no;
        s_count++;
        return true;
    }
    size_t w = weakest();                               // full: evict weakest if new is >=
    if (t->reinforce_count < s_store[w].reinforce_count) return false;
    s_store[w] = *t;
    s_store[w].last_seen_sweep = sweep_no;
    return true;
}

void learn_age_out(uint16_t sweep_no)
{
    for (size_t i = 0; i < s_count; ) {
        if ((uint16_t)(sweep_no - s_store[i].last_seen_sweep) > LEARN_AGEOUT_SWEEPS) {
            s_store[i] = s_store[--s_count];            // swap-remove
        } else i++;
    }
}
```

- [ ] **Step 5: Build + flash + read** — expect `SELFTEST: PASS` with `store:` checks.

- [ ] **Step 6: Commit**

```bash
git add main/learn.h main/learn.c main/churn_selftest.c
git commit -m "feat(learn): capped store with dedup, reinforce, weakest eviction, age-out"
```

---

### Task 5: Candidate pipeline — `learn_offer` / `learn_end_sweep`

Accumulate repeat sightings within a sweep; promote a shape to the store only after `LEARN_MIN_SIGHTINGS` clean sightings.

**Files:**
- Modify: `main/learn.h` (`learn_offer`, `learn_end_sweep`, `LEARN_CAND_SLOTS`, `LEARN_MIN_SIGHTINGS`)
- Modify: `main/learn.c`
- Modify: `main/churn_selftest.c` (`test_learn_pipeline()`)

**Interfaces:**
- Consumes: `learn_strip` (Task 2), `learn_shape_hash` (Task 3), `learn_store_add` (Task 4).
- Produces:
  - `void learn_offer(uint32_t mac_hash, const uint8_t *ad, uint8_t len, uint16_t company, uint32_t now_ms);` — updates the per-sweep candidate for `mac_hash`: on first offer strip + store the candidate (reject → ignore), record interval samples on repeats, and when its sighting count reaches `LEARN_MIN_SIGHTINGS`, promote via `learn_store_add` (once).
  - `void learn_end_sweep(uint16_t sweep_no);` — run `learn_age_out(sweep_no)` then wipe the candidate table.

- [ ] **Step 1: Write the failing test** (`test_learn_pipeline();`):

```c
static void test_learn_pipeline(void)
{
    learn_reset();
    uint8_t buds[] = { 0x02,0x01,0x06, 0x05,0xFF,0x75,0x00,0xAB,0xCD };
    uint32_t H = 0xBEEF1234;

    // K-1 sightings: not yet learned
    for (int i = 0; i < LEARN_MIN_SIGHTINGS - 1; i++)
        learn_offer(H, buds, sizeof buds, 0x0075, 100 * (i + 1));
    ST_CHECK(learn_count() == 0, "pipeline: below K not learned");

    // Kth sighting: promoted
    learn_offer(H, buds, sizeof buds, 0x0075, 100 * LEARN_MIN_SIGHTINGS);
    ST_CHECK(learn_count() == 1, "pipeline: reaching K promotes one template");

    // forbidden candidate never promoted regardless of repeats
    uint8_t nearby[] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    for (int i = 0; i < LEARN_MIN_SIGHTINGS + 2; i++)
        learn_offer(0xF00D, nearby, sizeof nearby, 0x004C, 50 * (i + 1));
    ST_CHECK(learn_count() == 1, "pipeline: forbidden never promoted");

    // end_sweep wipes candidates (a fresh sub-K burst next sweep won't promote)
    learn_end_sweep(1);
    learn_offer(H, buds, sizeof buds, 0x0075, 10);
    ST_CHECK(learn_count() == 1, "pipeline: post-sweep single sighting no new promote");
}
```

- [ ] **Step 2: Build to verify it fails** — `undefined reference to 'learn_offer'`.

- [ ] **Step 3: Add to `learn.h`:**

```c
#ifndef LEARN_MIN_SIGHTINGS
#define LEARN_MIN_SIGHTINGS 3
#endif
#ifndef LEARN_CAND_SLOTS
#define LEARN_CAND_SLOTS 24
#endif

void learn_offer(uint32_t mac_hash, const uint8_t *ad, uint8_t len,
                 uint16_t company, uint32_t now_ms);
void learn_end_sweep(uint16_t sweep_no);
```

- [ ] **Step 4: Implement in `learn.c`:**

```c
typedef struct {
    uint32_t          mac_hash;
    uint32_t          last_ms;
    uint16_t          sightings;
    bool              used;
    bool              promoted;
    uint16_t          itvl_min_ms, itvl_max_ms;
    learned_template_t skel;      // stripped once on first sighting; invalid if !valid
    bool              valid;      // strip succeeded
} learn_cand_t;

static learn_cand_t s_cand[LEARN_CAND_SLOTS];
static uint16_t     s_sweep;

void learn_offer(uint32_t mac_hash, const uint8_t *ad, uint8_t len,
                 uint16_t company, uint32_t now_ms)
{
    learn_cand_t *c = NULL, *freep = NULL;
    for (size_t i = 0; i < LEARN_CAND_SLOTS; i++) {
        if (s_cand[i].used && s_cand[i].mac_hash == mac_hash) { c = &s_cand[i]; break; }
        if (!s_cand[i].used && !freep) freep = &s_cand[i];
    }
    if (!c) {                                        // new candidate
        if (!freep) return;                          // table full: drop this sweep
        c = freep;
        memset(c, 0, sizeof(*c));
        c->used = true; c->mac_hash = mac_hash; c->last_ms = now_ms; c->sightings = 1;
        c->valid = learn_strip(ad, len, company, &c->skel);   // reject => valid=false
        return;
    }
    // repeat sighting: interval sample + count
    int32_t itvl = (int32_t)(now_ms - c->last_ms);
    if (itvl > 0 && itvl < 60000) {
        uint16_t v = (uint16_t)itvl;
        if (c->itvl_min_ms == 0 || v < c->itvl_min_ms) c->itvl_min_ms = v;
        if (v > c->itvl_max_ms) c->itvl_max_ms = v;
    }
    c->last_ms = now_ms;
    if (c->sightings < 0xFFFF) c->sightings++;
    if (c->valid && !c->promoted && c->sightings >= LEARN_MIN_SIGHTINGS) {
        c->skel.itvl_min_ms = c->itvl_min_ms ? c->itvl_min_ms : 100;
        c->skel.itvl_max_ms = c->itvl_max_ms ? c->itvl_max_ms : 200;
        c->skel.shape_hash  = learn_shape_hash(&c->skel);
        learn_store_add(&c->skel, s_sweep);
        c->promoted = true;
    }
}

void learn_end_sweep(uint16_t sweep_no)
{
    s_sweep = sweep_no;
    learn_age_out(sweep_no);
    memset(s_cand, 0, sizeof(s_cand));               // wipe transient candidates
}
```

> Note: `learn_reset()` must also clear the candidate table. Update it: `void learn_reset(void){ memset(s_store,0,sizeof(s_store)); s_count=0; memset(s_cand,0,sizeof(s_cand)); s_sweep=0; }` — move `learn_reset` below the `s_cand` definition, or forward-declare.

- [ ] **Step 5: Build + flash + read** — expect `SELFTEST: PASS` with `pipeline:` checks.

- [ ] **Step 6: Commit**

```bash
git add main/learn.h main/learn.c main/churn_selftest.c
git commit -m "feat(learn): candidate pipeline — K-sighting promotion within a sweep"
```

---

### Task 6: NVS persistence

Persist the learned store across reboots, mirroring `rf_model_save_nvs`/`load_nvs`.

**Files:**
- Modify: `main/learn.h` (`learn_save_nvs`, `learn_load_nvs`, magic/version)
- Modify: `main/learn.c`
- Modify: `main/churn_selftest.c` (`test_learn_nvs()`)

**Interfaces:**
- Produces:
  - `int learn_save_nvs(void);` — writes `{magic, version, count, s_store[0..count)}` blob under namespace `"splinter"`, key `"learn_db"`. Returns 0 on success.
  - `int learn_load_nvs(void);` — loads it into the RAM store; returns 0 only if a valid current-version blob exists, else leaves the store empty and returns nonzero.

- [ ] **Step 1: Write the failing test** (`test_learn_nvs();`, modeled on `test_detect_nvs`):

```c
static void test_learn_nvs(void)
{
    learn_reset();
    learned_template_t a; mk_shape(&a, 0x0075); learn_store_add(&a, 1);
    learned_template_t b; mk_shape(&b, 0x009E); learn_store_add(&b, 1);
    ST_CHECK(learn_count() == 2, "nvs: two before save");
    ST_CHECK(learn_save_nvs() == 0, "nvs: save ok");

    learn_reset();
    ST_CHECK(learn_count() == 0, "nvs: reset clears RAM");
    ST_CHECK(learn_load_nvs() == 0, "nvs: load ok");
    ST_CHECK(learn_count() == 2, "nvs: two restored");
    ST_CHECK(learn_at(0)->company_id == 0x0075, "nvs: entry round-trips");
}
```

- [ ] **Step 2: Build to verify it fails** — `undefined reference to 'learn_save_nvs'`.

- [ ] **Step 3: Add to `learn.h`:**

```c
#define LEARN_DB_MAGIC   0x4C524E31u   // "LRN1"
#define LEARN_DB_VERSION 1
int learn_save_nvs(void);
int learn_load_nvs(void);
```

- [ ] **Step 4: Implement in `learn.c`** (add `#include "nvs.h"`):

```c
// On-NVS layout: [uint32 magic][uint16 version][uint16 count][count * learned_template_t]
int learn_save_nvs(void)
{
    size_t bytes = 8 + s_count * sizeof(learned_template_t);
    static uint8_t buf[8 + LEARN_CAP * sizeof(learned_template_t)];
    uint32_t magic = LEARN_DB_MAGIC; uint16_t ver = LEARN_DB_VERSION, cnt = (uint16_t)s_count;
    memcpy(buf, &magic, 4); memcpy(buf + 4, &ver, 2); memcpy(buf + 6, &cnt, 2);
    memcpy(buf + 8, s_store, s_count * sizeof(learned_template_t));

    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READWRITE, &h);
    if (e != ESP_OK) return (int)e;
    e = nvs_set_blob(h, "learn_db", buf, bytes);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (int)e;
}

int learn_load_nvs(void)
{
    static uint8_t buf[8 + LEARN_CAP * sizeof(learned_template_t)];
    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READONLY, &h);
    if (e != ESP_OK) return (int)e;
    size_t len = sizeof(buf);
    e = nvs_get_blob(h, "learn_db", buf, &len);
    nvs_close(h);
    if (e != ESP_OK || len < 8) return -1;
    uint32_t magic; uint16_t ver, cnt;
    memcpy(&magic, buf, 4); memcpy(&ver, buf + 4, 2); memcpy(&cnt, buf + 6, 2);
    if (magic != LEARN_DB_MAGIC || ver != LEARN_DB_VERSION) return -1;
    if (cnt > LEARN_CAP || len != 8u + (size_t)cnt * sizeof(learned_template_t)) return -1;
    memcpy(s_store, buf + 8, (size_t)cnt * sizeof(learned_template_t));
    s_count = cnt;
    return 0;
}
```

- [ ] **Step 5: Build + flash + read** — expect `SELFTEST: PASS` with `nvs:` checks. (NVS persists between the flash and the reset-driven reread within the same test run.)

- [ ] **Step 6: Commit**

```bash
git add main/learn.h main/learn.c main/churn_selftest.c
git commit -m "feat(learn): NVS persistence for the learned store"
```

---

### Task 7: Generation integration

Let `generate.c` draw learned templates alongside the static ones, using the `archetype_idx` offset scheme, and keep the roster Law-3 invariants.

**Files:**
- Modify: `main/generate.c` (`build_for_vendor`, roster loop)
- Modify: `main/churn_selftest.c` (update `test_roster_generate_path`/`test_generate` archetype-bound assertions; add `test_learn_generate()`)

**Interfaces:**
- Consumes: `learn_count`, `learn_at`, `learn_render` (Tasks 3–4), `templates_count`.
- Produces: rosters whose `archetype_idx` may be `>= templates_count()`, meaning `learned[archetype_idx - templates_count()]`. All payloads remain non-empty, `<=31` B, Law-3-clean.

- [ ] **Step 1: Write the failing test** (`test_learn_generate();`) and fix the existing bound checks:

```c
static void test_learn_generate(void)
{
    // seed one learned Samsung shape
    learn_reset();
    uint8_t buds[] = { 0x02,0x01,0x06, 0x05,0xFF,0x75,0x00,0xAB,0xCD };
    for (int i = 0; i < LEARN_MIN_SIGHTINGS; i++)
        learn_offer(0xABCD, buds, sizeof buds, 0x0075, 100 * (i + 1));
    ST_CHECK(learn_count() == 1, "gen: one learned shape seeded");

    // a model with only Samsung observed -> roster should be buildable and clean
    rf_model_t m; rf_model_reset(&m);
    for (int i = 0; i < 60; i++) rf_model_observe(&m, 0x0075, -50, 0, 150);
    rf_model_end_sweep(&m, 5 /*distinct*/, 15000 /*window_ms*/, 5 /*arrivals*/);

    identity_t roster[8];
    size_t n = generate_roster(&m, roster, 8);
    ST_CHECK(n == 8, "gen: full roster built");
    bool clean = true, bound = true;
    for (size_t i = 0; i < 8; i++) {
        if (roster[i].payload_len == 0 || roster[i].payload_len > 31) clean = false;
        if (has_apple_popup_subtype(roster[i].payload, roster[i].payload_len)) clean = false;
        if (roster[i].archetype_idx >= templates_count() + learn_count()) bound = false;
    }
    ST_CHECK(clean, "gen: learned-inclusive roster payloads clean & in-budget");
    ST_CHECK(bound, "gen: archetype_idx within static+learned range");
}
```

> Also update the existing assertions in `test_roster_payloads`, `test_generate`, and `test_roster_generate_path` that read `archetype_idx >= templates_count()` (or `... < templates_count()`) to the new upper bound `templates_count() + learn_count()`.

- [ ] **Step 2: Build to verify it fails** — the new test fails its bound assertion (or the old assertions fire) because learned draw isn't wired.

- [ ] **Step 3: Wire learned templates into `build_for_vendor`** in `generate.c`. Add near the top after includes:

```c
#include "learn.h"
```

Insert, at the **start** of `build_for_vendor` (before the Apple / no-mfg / templated-vendor fallbacks), a chance to use a matching learned shape:

```c
    // Prefer a learned shape for this company when one exists (adds real-world variety).
    if (learn_count() > 0) {
        // collect learned indices matching this company (or any, for no-mfg)
        size_t cand[LEARN_CAP]; size_t k = 0;
        for (size_t i = 0; i < learn_count(); i++) {
            const learned_template_t *lt = learn_at(i);
            bool match = (company == RF_VENDOR_UNKNOWN) ? (lt->company_id == 0)
                                                        : (lt->company_id == company);
            if (match) cand[k++] = i;
        }
        if (k > 0) {
            size_t pick = cand[esp_random() % k];
            uint16_t itvl;
            if (learn_render(learn_at(pick), out, len, &itvl) == 0) {
                *arch_idx = (uint8_t)(templates_count() + pick);
                return 0;
            }
        }
    }
```

> `LEARN_CAP` is available via `#include "learn.h"`. `RF_VENDOR_UNKNOWN` is already included via `rf_model.h`. The interval sampled here is discarded (the roster loop samples interval from the model histogram as today); learned interval bands are still used when the learned template is rendered on the churn path — acceptable for Phase 1, the roster interval remains model-driven.

- [ ] **Step 4: Build + flash + read** — expect `SELFTEST: PASS` with `gen:` checks and all prior roster checks green.

- [ ] **Step 5: Commit**

```bash
git add main/generate.c main/churn_selftest.c
git commit -m "feat(learn): generation draws learned templates (archetype_idx offset)"
```

---

### Task 8: Live wiring + build gate

Hook the observe scan to `learn_offer`, advance sweeps, load/persist on boot — all behind `SIMULACRA_LEARN`. This is the only task that touches the live radio path.

**Files:**
- Modify: `main/observe.c` (call `learn_offer` in `observe_gap_event`; call `learn_end_sweep` in `observe_end_sweep`)
- Modify: `main/simulacra_main.c` (define `SIMULACRA_LEARN` guard; `learn_load_nvs()` on boot; periodic `learn_save_nvs()` piggybacking the observe persist)
- Modify: `main/observe.h` (if a sweep counter accessor is needed)

**Interfaces:**
- Consumes: `learn_offer`, `learn_end_sweep`, `learn_load_nvs`, `learn_save_nvs`.
- Produces: live harvesting during observe sweeps on real hardware.

- [ ] **Step 1: Add the guard to `simulacra_main.c`** near the other flag guards (`SIMULACRA_ESPNOW`):

```c
#ifndef SIMULACRA_LEARN
#define SIMULACRA_LEARN 1
#endif
```

- [ ] **Step 2: Hook `observe.c`.** Add `#include "learn.h"` and, in `observe_gap_event`, right after the existing `if (s_report_cb) s_report_cb(...)` line and before/at the `observe_ingest(...)` call:

```c
#if SIMULACRA_LEARN
    learn_offer(observe_hash_mac(d->addr.val), d->data, d->length_data, company, now);
#endif
```

Add a per-sweep counter and call `learn_end_sweep`. In `observe_end_sweep`, after `rf_model_end_sweep(...)`:

```c
#if SIMULACRA_LEARN
    static uint16_t s_learn_sweep;
    learn_end_sweep(++s_learn_sweep);
#endif
```

> `SIMULACRA_LEARN` is a compile flag; include its guard in `observe.c` too, or rely on it being defined project-wide. To keep `observe.c` self-contained, add the same `#ifndef SIMULACRA_LEARN / #define SIMULACRA_LEARN 1 / #endif` guard at the top of `observe.c`.

- [ ] **Step 3: Boot load + periodic save in `simulacra_main.c`.** Where `observe_start()` / the decoy observe path is initialized, add:

```c
#if SIMULACRA_LEARN
    learn_reset();
    learn_load_nvs();          // resume the library across reboots (empty if none)
#endif
```

And where the observe model is persisted (the `rf_model_save_nvs` tick), add alongside it:

```c
#if SIMULACRA_LEARN
    learn_save_nvs();
#endif
```

- [ ] **Step 4: Build the normal decoy firmware (no selftest) to confirm it compiles and links** for Ward:

```powershell
idf.py -B build.c5 build
```

Expected: `Project build complete`. Then build **with the flag off** to confirm the gate: `idf.py -B build.c5 -DSIMULACRA_LEARN=0 build` → also builds clean (learn calls compiled out).

- [ ] **Step 5: Selftest regression** — `idf.py -B build.c5 -DCHURN_SELFTEST=1 -p COM12 flash` + read. Expected: `SELFTEST: PASS (N/N)` (all learn tests + all prior).

- [ ] **Step 6: On-target live smoke (Ward, COM12).** Flash the normal firmware and read for ~60 s near real BLE devices (phone, earbuds), grepping for evidence the store grows. Add a one-line heartbeat log in `learn_store_add` on insert (`ESP_LOGW("learn","+shape company=0x%04X count=%u", t->company_id, (unsigned)s_count);`) if not already visible:

```powershell
idf.py -B build.c5 -p COM12 flash
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM12 --seconds 60 --reset yes --grep "learn|reprofile sweep"
```

Expected: at least one `+shape` line after a couple of sweeps in a normal room. (Environment-dependent; absence in an RF-quiet room is not a failure.)

- [ ] **Step 7: Commit**

```bash
git add main/observe.c main/observe.h main/simulacra_main.c
git commit -m "feat(learn): live observe->learn wiring behind SIMULACRA_LEARN"
```

---

## Self-Review

**Spec coverage:**
- Capture hook (spec §1) → Task 8. Candidate table / K-sightings (spec §2) → Task 5. Identity-strip + Law-3 gate (spec §3) → Tasks 1–2. `shape_hash` dedup + render (spec §3–4) → Task 3. Store + lifecycle (spec §4–5) → Task 4. NVS (spec §4) → Task 6. Generation integration + `archetype_idx` (spec §6) → Task 7. Testing (spec §Testing) → per-task `ST_CHECK` tests. Config/gates (spec §Configuration) → Task 8 + `learn.h` tunables. Forward-compat POD (spec §Forward-compatibility) → `learned_template_t` in Task 2.
- **Gap addressed:** the spec's on-target soak is Task 8 Step 6.

**Placeholder scan:** every code step contains full code. No TBD/TODO. Build/flash/read commands are explicit.

**Type consistency:** `learned_template_t` fields are defined once (Task 2) and referenced consistently. `learn_offer` signature matches between Task 5 definition and Task 8 call site (`mac_hash, ad, len, company, now_ms`). `learn_store_add(t, sweep_no)`, `learn_render(t, out, len, itvl)`, `learn_shape_hash(t)`, `learn_age_out(sweep_no)`, `learn_end_sweep(sweep_no)` are consistent across tasks. `archetype_idx` offset (`templates_count() + i`) is used identically in Task 7 wiring and its assertions.

**Note for the implementer:** `learn_reset()` is introduced in Task 4 but must also clear the candidate table added in Task 5 — Task 5 Step 4 calls this out explicitly.
