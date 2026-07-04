# BLE Threat Fingerprinting (Slice 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recognize BLE tracker *classes* (AirTag/SmartTag/Tile) by RF signature on the decoy and surface them as typed `KNOWN` threats on Vigil, with the signature DB custodied encrypted on Vigil's SD and pushed to decoys over ESP-NOW.

**Architecture:** The inverted `learn.db` librarian. A packed `threat_sig_t` record + a pure masked-pattern matcher run on the decoy at the observe hook; hits fold into `detect.c`'s shared threat table as a new `KNOWN` kind. Vigil seals a versioned signature DB to SD (mirroring `learn_db`) and broadcasts it over ESP-NOW (mirroring `learn_wire`); decoys re-gate every received record and swap their RAM DB wholesale.

**Tech Stack:** ESP-IDF v5.5 (C5 decoy selftest, COM12) / v5.4 (CYD Vigil, COM10). Shared code in `components/simulacra_radar/`. On-target selftest harness `main/churn_selftest.c` (`ST_CHECK(cond,msg)`; add `test_*` fns and register them in `churn_selftest_run()`).

## Global Constraints

- **Law-3 / data discipline:** a hit stores a category/class, never a raw identity; storage is hash-only (like `detect_threat_t`).
- **Never trust the wire:** `sig_regate` runs on every received signature before it enters RAM; the matcher also bounds-checks every compare. Worst case from a bad record is a missed/spurious match — never an OOB read.
- **Packed, pointer-free PODs:** `threat_sig_t` is `__attribute__((packed))`, wire- and SD-ready.
- **Paired builds:** decoy + Vigil are always flashed together; the persisted+wire threat-struct format bump is coordinated. Bump the version regardless.
- **Detection/awareness only:** never interference, jamming, or evasion. A hit means "class present in the area," not "following you."
- **Honesty in-product:** every hit carries a `confidence` tier; the UI qualifies it (`likely`/`possible`).
- **Build gates:** shared `.c` files must be added to `components/simulacra_radar/CMakeLists.txt` SRCS. Selftest build = `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; **normal builds must pass `-DCHURN_SELFTEST=0` explicitly** (stale CMake-cache gotcha: an old `-DCHURN_SELFTEST=1` persists otherwise).
- **Commits carry AI trailers → scrub before any push.** `main` is local-only.
- **New ESP-NOW frame type:** `RADAR_TYPE_SIG_SYNC = 5` (learn uses 3/4).

## File Structure

Shared — `components/simulacra_radar/` (compiles for both classic-ESP32 Vigil / IDF 5.4 and C5/C6 decoy):
- `threat_sig.h` — `threat_sig_t`, enums/constants, `sig_adv_fields_t`, `sig_hit_t`. (header only)
- `sig_match.{h,c}` — pure matcher `sig_match`, `sig_addr_type_from`, `sig_regate`.
- `sig_db.{h,c}` — `sig_db_derive_key`, `sig_db_seal`, `sig_db_open` (clone of `learn_db`).
- `sig_wire.{h,c}` — `RADAR_TYPE_SIG_SYNC`, `sig_chunk_hdr_t`, `sig_wire_pack`, `sig_wire_unpack`.
- `sig_seed.{h,c}` — compile-time seed set + `sig_seed_version/count/copy`.
- `sig_class_name.h` — `sig_class_name(uint8_t class_id)`. (header only)

Decoy — `main/`:
- `detect.{h,c}` — extend `detect_threat_t`; `detect_note_known`; follower-priority eviction; NVS magic bump.
- `sig_store.{h,c}` — decoy RAM signature DB: seed load, read accessors, `sig_store_adopt` (re-gate + wholesale swap).
- `observe.c` — build `sig_adv_fields_t`, call `sig_match` → `detect_note_known`.
- `esp_now_link.c` — receive `SIG_SYNC`, reassemble a version's chunks, `sig_store_adopt`.

Vigil — `cyd/main/cyd_main.c`: load/seal card DB, broadcast `SIG_SYNC` every 60 s, render KNOWN hits.
Shared wire — `components/simulacra_radar/radar_wire.h`: extend the per-threat status element.
Shared render — `components/simulacra_radar/radar_render.c`: label + color KNOWN threats.

---

### Task 1: `threat_sig.h` + core matcher

**Files:**
- Create: `components/simulacra_radar/threat_sig.h`, `components/simulacra_radar/sig_match.h`, `components/simulacra_radar/sig_match.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `sig_match.c`)
- Test: `main/churn_selftest.c` (`test_sig_match`)

**Interfaces:**
- Produces:
  - `threat_sig_t` (packed, ~45 B), enums `sig_category_t`/`sig_class_t`/`sig_src_t`, `SIG_PAT_MAX 16`, `SIG_ADDR_*`, `SIG_DB_CAP 64`.
  - `sig_adv_fields_t { uint16_t company_id; uint16_t svc_uuid16; uint8_t addr_type; const uint8_t *mfg_data; uint8_t mfg_len; const uint8_t *svc_data; uint8_t svc_len; }`
  - `sig_hit_t { uint16_t sig_id; uint8_t category; uint8_t class_id; uint8_t confidence; }`
  - `bool sig_match(const sig_adv_fields_t *adv, const threat_sig_t *db, size_t count, sig_hit_t *out);`
  - `uint8_t sig_addr_type_from(uint8_t nimble_addr_type, const uint8_t mac[6]);`

- [ ] **Step 1: Write the failing test**

Add to `main/churn_selftest.c` (near the learn tests), and register `test_sig_match();` in `churn_selftest_run()`:

```c
#include "threat_sig.h"
#include "sig_match.h"

static void test_sig_match(void)
{
    // One signature: Apple company 0x004C, mfg-data type byte 0x12 at offset 2, length byte masked in.
    threat_sig_t s = {0};
    s.sig_id = 1; s.category = SIG_CAT_TRACKER; s.class_id = SIG_CLASS_AIRTAG;
    s.company_id = 0x004C; s.svc_uuid16 = 0x0000;
    s.addr_type_mask = SIG_ADDR_RPA | SIG_ADDR_NRPA;
    s.match_src = SIG_SRC_MFG_DATA; s.pat_off = 2; s.pat_len = 1;
    s.pattern[0] = 0x12; s.mask[0] = 0xFF; s.confidence = 80;

    // Advert: 4C 00 12 19 <rotating…>  (company 0x004C, type 0x12, len 0x19, key bytes vary)
    uint8_t mfg[] = { 0x4C,0x00, 0x12, 0x19, 0xAA,0xBB,0xCC };
    sig_adv_fields_t adv = { .company_id = 0x004C, .svc_uuid16 = 0,
        .addr_type = SIG_ADDR_RPA, .mfg_data = mfg, .mfg_len = sizeof mfg,
        .svc_data = NULL, .svc_len = 0 };

    sig_hit_t hit;
    ST_CHECK(sig_match(&adv, &s, 1, &hit), "sig_match: airtag advert hits");
    ST_CHECK(hit.class_id == SIG_CLASS_AIRTAG, "sig_match: reports class");

    // Rotating key bytes changed -> still hits (mask ignores them).
    mfg[4] = 0x11; mfg[5] = 0x22;
    ST_CHECK(sig_match(&adv, &s, 1, &hit), "sig_match: robust to rotating bytes");

    // A must-match byte changed -> no hit.
    uint8_t mfg2[] = { 0x4C,0x00, 0x07, 0x19, 0x00 };
    adv.mfg_data = mfg2; adv.mfg_len = sizeof mfg2;
    ST_CHECK(!sig_match(&adv, &s, 1, &hit), "sig_match: near-miss rejected");

    // Wrong company -> no hit.
    adv.mfg_data = mfg; adv.mfg_len = sizeof mfg; adv.company_id = 0x0075;
    ST_CHECK(!sig_match(&adv, &s, 1, &hit), "sig_match: company gate");

    // Short advert at a big offset -> no hit, no OOB.
    adv.company_id = 0x004C; adv.mfg_len = 2;
    ST_CHECK(!sig_match(&adv, &s, 1, &hit), "sig_match: short advert bounds-safe");

    // Address-type gate: public addr rejected for an RPA/NRPA-only signature.
    adv.mfg_len = sizeof mfg; adv.addr_type = SIG_ADDR_PUBLIC;
    ST_CHECK(!sig_match(&adv, &s, 1, &hit), "sig_match: addr-type gate");

    // sig_addr_type_from: random MSB 0b01 = RPA, 0b11 = static, public type = public.
    uint8_t rpa[6]  = {0,0,0,0,0, 0x40};
    uint8_t stat[6] = {0,0,0,0,0, 0xC0};
    ST_CHECK(sig_addr_type_from(1, rpa)  == SIG_ADDR_RPA,    "addr_type: RPA");
    ST_CHECK(sig_addr_type_from(1, stat) == SIG_ADDR_STATIC, "addr_type: static");
    ST_CHECK(sig_addr_type_from(0, rpa)  == SIG_ADDR_PUBLIC, "addr_type: public");
}
```

- [ ] **Step 2: Run selftest build to verify it fails**

Run: `idf.py -DCHURN_SELFTEST=1 -p COM12 build`
Expected: compile FAIL — `threat_sig.h` / `sig_match` not found.

- [ ] **Step 3: Create `threat_sig.h`**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SIG_PAT_MAX 16
#define SIG_DB_CAP  64          // RAM working set on both roles

typedef enum { SIG_CAT_TRACKER = 0, SIG_CAT_CAMERA, SIG_CAT_BODYCAM, SIG_CAT_UNKNOWN, SIG_CAT_COUNT } sig_category_t;
typedef enum { SIG_CLASS_AIRTAG = 0, SIG_CLASS_SMARTTAG, SIG_CLASS_TILE, SIG_CLASS_COUNT } sig_class_t;
typedef enum { SIG_SRC_MFG_DATA = 0, SIG_SRC_SVC_DATA, SIG_SRC_COUNT } sig_src_t;

#define SIG_ADDR_PUBLIC  0x01
#define SIG_ADDR_STATIC  0x02   // random static
#define SIG_ADDR_RPA     0x04   // resolvable private (rotating)
#define SIG_ADDR_NRPA    0x08   // non-resolvable private

typedef struct __attribute__((packed)) {
    uint16_t sig_id;
    uint8_t  category;          // sig_category_t
    uint8_t  class_id;          // sig_class_t
    uint16_t company_id;        // required mfg company id; 0xFFFF = don't care
    uint16_t svc_uuid16;        // required 16-bit service UUID; 0x0000 = don't care
    uint8_t  addr_type_mask;    // SIG_ADDR_* bitmask; 0 = any
    uint8_t  match_src;         // sig_src_t
    uint8_t  pat_off;           // offset into that AD payload
    uint8_t  pat_len;           // pattern length (<= SIG_PAT_MAX)
    uint8_t  pattern[SIG_PAT_MAX];
    uint8_t  mask[SIG_PAT_MAX];
    uint8_t  confidence;        // 0..100
} threat_sig_t;

typedef struct {
    uint16_t company_id;        // 0xFFFF if no mfg data
    uint16_t svc_uuid16;        // 0x0000 if none
    uint8_t  addr_type;         // one SIG_ADDR_* bit
    const uint8_t *mfg_data; uint8_t mfg_len;   // includes the 2-byte company id
    const uint8_t *svc_data; uint8_t svc_len;   // includes the 2-byte service uuid
} sig_adv_fields_t;

typedef struct { uint16_t sig_id; uint8_t category; uint8_t class_id; uint8_t confidence; } sig_hit_t;
```

- [ ] **Step 4: Create `sig_match.h`**

```c
#pragma once
#include "threat_sig.h"

// Returns true iff any signature matches; fills *out with the first match.
bool sig_match(const sig_adv_fields_t *adv, const threat_sig_t *db, size_t count, sig_hit_t *out);

// Map a NimBLE addr type + raw MAC to one SIG_ADDR_* bit. Random addresses are
// classified by the top two bits of the MSB (val[5]): 0b11 static, 0b01 RPA, 0b00 NRPA.
uint8_t sig_addr_type_from(uint8_t nimble_addr_type, const uint8_t mac[6]);

// Re-validate a signature before trusting it (bounds + range). true = safe to use.
bool sig_regate(const threat_sig_t *s);
```

- [ ] **Step 5: Create `sig_match.c`**

```c
#include "sig_match.h"
#include <string.h>

uint8_t sig_addr_type_from(uint8_t nimble_addr_type, const uint8_t mac[6])
{
    // NimBLE: 0/2 = public(-id), 1/3 = random(-id).
    if (nimble_addr_type == 0 || nimble_addr_type == 2) return SIG_ADDR_PUBLIC;
    switch (mac[5] >> 6) {
        case 0x3: return SIG_ADDR_STATIC;
        case 0x1: return SIG_ADDR_RPA;
        case 0x0: return SIG_ADDR_NRPA;
        default:  return SIG_ADDR_STATIC;   // 0b10 reserved -> treat as static
    }
}

bool sig_regate(const threat_sig_t *s)
{
    if (!s) return false;
    if (s->pat_len > SIG_PAT_MAX) return false;
    if ((int)s->pat_off + (int)s->pat_len > SIG_PAT_MAX) return false;  // pattern[]/mask[] bound
    if (s->category >= SIG_CAT_COUNT) return false;
    if (s->class_id >= SIG_CLASS_COUNT) return false;
    if (s->match_src >= SIG_SRC_COUNT) return false;
    if (s->confidence > 100) return false;
    return true;
}

static bool one_match(const sig_adv_fields_t *adv, const threat_sig_t *s)
{
    if (s->company_id != 0xFFFF && adv->company_id != s->company_id) return false;
    if (s->svc_uuid16 != 0x0000 && adv->svc_uuid16 != s->svc_uuid16) return false;
    if (s->addr_type_mask != 0 && !(s->addr_type_mask & adv->addr_type)) return false;

    const uint8_t *buf; uint8_t len;
    if (s->match_src == SIG_SRC_MFG_DATA) { buf = adv->mfg_data; len = adv->mfg_len; }
    else                                  { buf = adv->svc_data; len = adv->svc_len; }
    if (!buf) return false;
    if ((int)s->pat_off + (int)s->pat_len > (int)len) return false;   // bounds-safe
    for (uint8_t i = 0; i < s->pat_len; i++)
        if ((buf[s->pat_off + i] & s->mask[i]) != (s->pattern[i] & s->mask[i])) return false;
    return true;
}

bool sig_match(const sig_adv_fields_t *adv, const threat_sig_t *db, size_t count, sig_hit_t *out)
{
    if (!adv || !db) return false;
    for (size_t i = 0; i < count; i++) {
        if (one_match(adv, &db[i])) {
            if (out) { out->sig_id = db[i].sig_id; out->category = db[i].category;
                       out->class_id = db[i].class_id; out->confidence = db[i].confidence; }
            return true;
        }
    }
    return false;
}
```

Add `"sig_match.c"` to `components/simulacra_radar/CMakeLists.txt` SRCS.

- [ ] **Step 6: Run selftest to verify it passes**

Run: `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`
Expected: `SELFTEST result: PASS`, count grows by 10.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/threat_sig.h components/simulacra_radar/sig_match.h components/simulacra_radar/sig_match.c components/simulacra_radar/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(sig): threat_sig record + pure masked-pattern matcher"
```

---

### Task 2: `sig_regate` coverage

**Files:**
- Test: `main/churn_selftest.c` (`test_sig_regate`)

**Interfaces:**
- Consumes: `sig_regate` (created in Task 1).

- [ ] **Step 1: Write the failing test** (register `test_sig_regate();`)

```c
static void test_sig_regate(void)
{
    threat_sig_t s = {0};
    s.category = SIG_CAT_TRACKER; s.class_id = SIG_CLASS_TILE; s.match_src = SIG_SRC_SVC_DATA;
    s.pat_off = 0; s.pat_len = 2; s.confidence = 75;
    ST_CHECK(sig_regate(&s), "regate: well-formed accepted");

    threat_sig_t b1 = s; b1.pat_len = SIG_PAT_MAX + 1;
    ST_CHECK(!sig_regate(&b1), "regate: over-long pattern rejected");
    threat_sig_t b2 = s; b2.pat_off = SIG_PAT_MAX - 1; b2.pat_len = 4;   // off+len overruns pattern[]
    ST_CHECK(!sig_regate(&b2), "regate: offset+len overrun rejected");
    threat_sig_t b3 = s; b3.category = SIG_CAT_COUNT;
    ST_CHECK(!sig_regate(&b3), "regate: bad category rejected");
    threat_sig_t b4 = s; b4.class_id = SIG_CLASS_COUNT;
    ST_CHECK(!sig_regate(&b4), "regate: bad class rejected");
    threat_sig_t b5 = s; b5.match_src = SIG_SRC_COUNT;
    ST_CHECK(!sig_regate(&b5), "regate: bad match_src rejected");
    threat_sig_t b6 = s; b6.confidence = 101;
    ST_CHECK(!sig_regate(&b6), "regate: confidence>100 rejected");
}
```

- [ ] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (function present but test unregistered → actually it compiles; run and expect the new checks; if `sig_regate` already handles all, they PASS — that's fine, this task documents the contract). If any check FAILS, fix `sig_regate` in `sig_match.c`.

- [ ] **Step 3: Confirm implementation** — `sig_regate` from Task 1 already enforces every case above. No code change expected.

- [ ] **Step 4: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS, +7.

- [ ] **Step 5: Commit**

```bash
git add main/churn_selftest.c
git commit -m "test(sig): re-gate bounds/range coverage"
```

---

### Task 3: `sig_db` encrypted seal/open

**Files:**
- Create: `components/simulacra_radar/sig_db.h`, `components/simulacra_radar/sig_db.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `sig_db.c`)
- Test: `main/churn_selftest.c` (`test_sig_db_blob`)

**Interfaces:**
- Produces:
  - `void sig_db_derive_key(const uint8_t psk[32], uint8_t out_key[32]);`
  - `int sig_db_seal(uint8_t *out, size_t *out_len, const threat_sig_t *recs, uint16_t count, uint16_t content_version, const uint8_t key[32]);`
  - `int sig_db_open(const uint8_t *buf, size_t len, threat_sig_t *recs, uint16_t *count, uint16_t *content_version, const uint8_t key[32]);`
  - `SIG_DB_MAGIC 0x53494731u` ("SIG1"), `SIG_DB_FMT_VER 1`, `sig_db_hdr_t`.

**Reference:** clone `components/simulacra_radar/learn_db.{h,c}` exactly; substitute the record type (`threat_sig_t`), magic, label `"simulacra-sigdb-v1"`, and add `content_version` to the header (authenticated as AAD alongside magic/version/count).

- [ ] **Step 1: Write the failing test** (register `test_sig_db_blob();`)

```c
#include "sig_db.h"

static void test_sig_db_blob(void)
{
    uint8_t psk[32]; for (int i=0;i<32;i++) psk[i]=(uint8_t)(i*7+1);
    uint8_t key[32]; sig_db_derive_key(psk, key);

    threat_sig_t recs[2] = {0};
    recs[0].sig_id = 1; recs[0].class_id = SIG_CLASS_AIRTAG; recs[0].company_id = 0x004C;
    recs[1].sig_id = 2; recs[1].class_id = SIG_CLASS_TILE;   recs[1].svc_uuid16 = 0xFEED;

    static uint8_t blob[sizeof(sig_db_hdr_t) + 2*sizeof(threat_sig_t)]; size_t blen;
    ST_CHECK(sig_db_seal(blob, &blen, recs, 2, 7, key) == 0, "sigdb: seal ok");

    threat_sig_t out[2]; uint16_t cnt = 0, ver = 0;
    ST_CHECK(sig_db_open(blob, blen, out, &cnt, &ver, key) == 0, "sigdb: open ok");
    ST_CHECK(cnt == 2 && ver == 7, "sigdb: count + content_version recovered");
    ST_CHECK(out[1].svc_uuid16 == 0xFEED, "sigdb: record bytes intact");

    blob[blen/2] ^= 0xFF;                       // corrupt ciphertext
    ST_CHECK(sig_db_open(blob, blen, out, &cnt, &ver, key) != 0, "sigdb: tamper rejected");

    uint8_t key2[32]; psk[0]^=1; sig_db_derive_key(psk, key2);
    ST_CHECK(sig_db_open(blob, blen, out, &cnt, &ver, key2) != 0, "sigdb: wrong key rejected");
}
```

- [ ] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (`sig_db.h` missing).

- [ ] **Step 3: Create `sig_db.h`**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "threat_sig.h"

#define SIG_DB_LABEL "simulacra-sigdb-v1"
void sig_db_derive_key(const uint8_t psk[32], uint8_t out_key[32]);

#define SIG_DB_MAGIC   0x53494731u   // "SIG1"
#define SIG_DB_FMT_VER 1

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format_version;
    uint16_t content_version;
    uint16_t count;
    uint8_t  nonce[12];
    uint8_t  tag[16];
} sig_db_hdr_t;

int sig_db_seal(uint8_t *out, size_t *out_len, const threat_sig_t *recs,
                uint16_t count, uint16_t content_version, const uint8_t key[32]);
int sig_db_open(const uint8_t *buf, size_t len, threat_sig_t *recs,
                uint16_t *count, uint16_t *content_version, const uint8_t key[32]);
```

- [ ] **Step 4: Create `sig_db.c`** — copy `learn_db.c` and adapt. Key points: HKDF-SHA256 with `SIG_DB_LABEL` (reuse `learn_db.c`'s HMAC-based HKDF verbatim — `mbedtls_hkdf` is not compiled in default IDF); authenticate `magic|format_version|content_version|count` as GCM AAD; ciphertext = `count * sizeof(threat_sig_t)`; random 12-byte nonce; 16-byte tag. `sig_db_open` verifies magic + `format_version == SIG_DB_FMT_VER`, checks `len == sizeof(hdr) + count*sizeof(threat_sig_t)`, authenticates, then copies out records + `*content_version`.

Add `"sig_db.c"` to `components/simulacra_radar/CMakeLists.txt` SRCS.

- [ ] **Step 5: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS, +6.

- [ ] **Step 6: Commit**

```bash
git add components/simulacra_radar/sig_db.h components/simulacra_radar/sig_db.c components/simulacra_radar/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(sig): AES-256-GCM sealed signature DB (content-versioned)"
```

---

### Task 4: `sig_wire` chunk framing

**Files:**
- Create: `components/simulacra_radar/sig_wire.h`, `components/simulacra_radar/sig_wire.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `sig_wire.c`)
- Test: `main/churn_selftest.c` (`test_sig_wire`)

**Interfaces:**
- Produces:
  - `#define RADAR_TYPE_SIG_SYNC 5`, `#define SIG_WIRE_RECS_PER_CHUNK 4`
  - `sig_chunk_hdr_t { uint16_t content_version; uint8_t chunk_index; uint8_t chunk_count; uint8_t rec_count; }`
  - `int sig_wire_pack(uint8_t *payload, size_t *plen, const threat_sig_t *recs, uint8_t nrecs, uint16_t content_version, uint8_t chunk_index, uint8_t chunk_count);`
  - `int sig_wire_unpack(const uint8_t *payload, size_t plen, threat_sig_t *recs, uint8_t *nrecs, sig_chunk_hdr_t *hdr);`

**Reference:** mirror `learn_wire.c`'s `learn_wire_pack`/`learn_wire_unpack` (header + packed records; validate `rec_count <= SIG_WIRE_RECS_PER_CHUNK` and `plen` on unpack).

- [ ] **Step 1: Write the failing test** (register `test_sig_wire();`)

```c
#include "sig_wire.h"

static void test_sig_wire(void)
{
    threat_sig_t recs[3] = {0};
    recs[0].sig_id = 10; recs[1].sig_id = 11; recs[2].sig_id = 12;
    recs[2].company_id = 0x0075;

    uint8_t pl[RADAR_FRAME_MAX]; size_t plen;
    ST_CHECK(sig_wire_pack(pl, &plen, recs, 3, 42, 0, 2) == 0, "sigwire: pack ok");
    ST_CHECK(plen <= 218, "sigwire: chunk fits");

    threat_sig_t out[SIG_WIRE_RECS_PER_CHUNK]; uint8_t nr; sig_chunk_hdr_t h;
    ST_CHECK(sig_wire_unpack(pl, plen, out, &nr, &h) == 0, "sigwire: unpack ok");
    ST_CHECK(nr == 3 && h.content_version == 42 && h.chunk_count == 2, "sigwire: hdr recovered");
    ST_CHECK(out[2].company_id == 0x0075, "sigwire: record bytes intact");

    ST_CHECK(sig_wire_pack(pl, &plen, recs, SIG_WIRE_RECS_PER_CHUNK + 1, 1, 0, 1) < 0,
             "sigwire: over-capacity pack rejected");
    ST_CHECK(sig_wire_unpack(pl, 2, out, &nr, &h) < 0, "sigwire: short payload rejected");
}
```

- [ ] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (`sig_wire.h` missing).

- [ ] **Step 3: Create `sig_wire.h` + `sig_wire.c`** — mirror `learn_wire`. `sig_wire.h`:

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "threat_sig.h"

#define RADAR_TYPE_SIG_SYNC 5          // Vigil -> all: signature DB chunk
#define SIG_WIRE_RECS_PER_CHUNK 4      // (218 - hdr) / sizeof(threat_sig_t)

typedef struct __attribute__((packed)) {
    uint16_t content_version;
    uint8_t  chunk_index;
    uint8_t  chunk_count;
    uint8_t  rec_count;
} sig_chunk_hdr_t;

int sig_wire_pack(uint8_t *payload, size_t *plen, const threat_sig_t *recs, uint8_t nrecs,
                  uint16_t content_version, uint8_t chunk_index, uint8_t chunk_count);
int sig_wire_unpack(const uint8_t *payload, size_t plen, threat_sig_t *recs,
                    uint8_t *nrecs, sig_chunk_hdr_t *hdr);
```

`sig_wire.c` — pack writes `sig_chunk_hdr_t` then `nrecs * sizeof(threat_sig_t)`; returns `<0` if `nrecs > SIG_WIRE_RECS_PER_CHUNK`. unpack validates `plen >= sizeof(hdr)`, reads hdr, validates `rec_count <= SIG_WIRE_RECS_PER_CHUNK` and `plen == sizeof(hdr) + rec_count*sizeof(threat_sig_t)`, copies records. Add `"sig_wire.c"` to CMakeLists SRCS.

- [ ] **Step 4: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS, +6.

- [ ] **Step 5: Commit**

```bash
git add components/simulacra_radar/sig_wire.h components/simulacra_radar/sig_wire.c components/simulacra_radar/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(sig): ESP-NOW chunk framing for signature DB sync"
```

---

### Task 5: seed signatures + `sig_class_name.h`

**Files:**
- Create: `components/simulacra_radar/sig_seed.h`, `components/simulacra_radar/sig_seed.c`, `components/simulacra_radar/sig_class_name.h`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `sig_seed.c`)
- Test: `main/churn_selftest.c` (`test_sig_seed`)

**Interfaces:**
- Produces:
  - `uint16_t sig_seed_version(void);`
  - `size_t   sig_seed_count(void);`
  - `size_t   sig_seed_copy(threat_sig_t *out, size_t max);`  // returns min(count,max)
  - `const char *sig_class_name(uint8_t class_id);`

**Note on byte values:** the pattern/offset/mask below are the implementer's starting vectors from public documentation. They make the tests self-consistent; **confirm against a live capture** (run the decoy in observe/sniff mode near a real AirTag/SmartTag/Tile and compare) before the bench sign-off in Task 12.

- [ ] **Step 1: Write the failing test** (register `test_sig_seed();`)

```c
#include "sig_seed.h"
#include "sig_class_name.h"

static void test_sig_seed(void)
{
    threat_sig_t db[SIG_DB_CAP];
    size_t n = sig_seed_copy(db, SIG_DB_CAP);
    ST_CHECK(n >= 3, "seed: at least airtag/smarttag/tile");
    ST_CHECK(sig_seed_version() >= 1, "seed: content_version set");
    for (size_t i = 0; i < n; i++) ST_CHECK(sig_regate(&db[i]), "seed: every record re-gates clean");

    // An AirTag-shaped advert hits the seed as AIRTAG.
    uint8_t mfg[] = { 0x4C,0x00, 0x12, 0x19, 0x10,0x00,0x00 };
    sig_adv_fields_t adv = { .company_id = 0x004C, .svc_uuid16 = 0, .addr_type = SIG_ADDR_RPA,
        .mfg_data = mfg, .mfg_len = sizeof mfg, .svc_data = NULL, .svc_len = 0 };
    sig_hit_t hit;
    ST_CHECK(sig_match(&adv, db, n, &hit) && hit.class_id == SIG_CLASS_AIRTAG, "seed: airtag hit");

    // A Tile-shaped service-data advert hits as TILE.
    uint8_t svc[] = { 0xED,0xFE, 0x02,0x00,0x0C };   // 0xFEED service data (LE uuid) + payload
    sig_adv_fields_t tadv = { .company_id = 0xFFFF, .svc_uuid16 = 0xFEED, .addr_type = SIG_ADDR_PUBLIC,
        .mfg_data = NULL, .mfg_len = 0, .svc_data = svc, .svc_len = sizeof svc };
    ST_CHECK(sig_match(&tadv, db, n, &hit) && hit.class_id == SIG_CLASS_TILE, "seed: tile hit");

    ST_CHECK(sig_class_name(SIG_CLASS_AIRTAG)[0] != '\0', "class name: airtag non-empty");
}
```

- [ ] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (`sig_seed.h` missing).

- [ ] **Step 3: Create `sig_class_name.h`**

```c
#pragma once
#include "threat_sig.h"
static inline const char *sig_class_name(uint8_t class_id)
{
    switch (class_id) {
        case SIG_CLASS_AIRTAG:   return "AirTag";
        case SIG_CLASS_SMARTTAG: return "SmartTag";
        case SIG_CLASS_TILE:     return "Tile";
        default:                 return "?";
    }
}
```

- [ ] **Step 4: Create `sig_seed.h` + `sig_seed.c`**

`sig_seed.h`:

```c
#pragma once
#include <stddef.h>
#include "threat_sig.h"
uint16_t sig_seed_version(void);
size_t   sig_seed_count(void);
size_t   sig_seed_copy(threat_sig_t *out, size_t max);   // returns min(count, max)
```

`sig_seed.c`:

```c
#include "sig_seed.h"
#include <string.h>

#define SIG_SEED_VERSION 1

static const threat_sig_t SEED[] = {
    // AirTag / Find My offline finding: Apple 0x004C, mfg type byte 0x12 at off 2 (key bytes masked).
    { .sig_id=1, .category=SIG_CAT_TRACKER, .class_id=SIG_CLASS_AIRTAG,
      .company_id=0x004C, .svc_uuid16=0x0000, .addr_type_mask=SIG_ADDR_RPA|SIG_ADDR_NRPA,
      .match_src=SIG_SRC_MFG_DATA, .pat_off=2, .pat_len=1,
      .pattern={0x12}, .mask={0xFF}, .confidence=80 },
    // Samsung SmartTag: service UUID 0xFD5A (Samsung); may also appear on other Samsung gear -> "possible".
    { .sig_id=2, .category=SIG_CAT_TRACKER, .class_id=SIG_CLASS_SMARTTAG,
      .company_id=0xFFFF, .svc_uuid16=0xFD5A, .addr_type_mask=0,
      .match_src=SIG_SRC_SVC_DATA, .pat_off=0, .pat_len=2,
      .pattern={0x5A,0xFD}, .mask={0xFF,0xFF}, .confidence=70 },
    // Tile: service UUID 0xFEED.
    { .sig_id=3, .category=SIG_CAT_TRACKER, .class_id=SIG_CLASS_TILE,
      .company_id=0xFFFF, .svc_uuid16=0xFEED, .addr_type_mask=0,
      .match_src=SIG_SRC_SVC_DATA, .pat_off=0, .pat_len=2,
      .pattern={0xED,0xFE}, .mask={0xFF,0xFF}, .confidence=75 },
};

uint16_t sig_seed_version(void) { return SIG_SEED_VERSION; }
size_t   sig_seed_count(void)   { return sizeof(SEED)/sizeof(SEED[0]); }
size_t   sig_seed_copy(threat_sig_t *out, size_t max)
{
    size_t n = sig_seed_count(); if (n > max) n = max;
    memcpy(out, SEED, n * sizeof(threat_sig_t));
    return n;
}
```

Add `"sig_seed.c"` to CMakeLists SRCS.

- [ ] **Step 5: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS, +7.

- [ ] **Step 6: Commit**

```bash
git add components/simulacra_radar/sig_seed.h components/simulacra_radar/sig_seed.c components/simulacra_radar/sig_class_name.h components/simulacra_radar/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(sig): seed signature set (AirTag/SmartTag/Tile) + class names"
```

---

### Task 6: `detect.c` KNOWN kind + `detect_note_known`

**Files:**
- Modify: `main/detect.h` (extend `detect_threat_t`, add `DETECT_KIND_*` + `detect_note_known`)
- Modify: `main/detect.c` (fields, `detect_note_known`, eviction priority, NVS magic bump)
- Test: `main/churn_selftest.c` (`test_detect_known`)

**Interfaces:**
- Produces:
  - `#define DETECT_KIND_FOLLOWER 0`, `#define DETECT_KIND_KNOWN 1`
  - `detect_threat_t` gains `uint8_t kind, class_id, category, confidence;`
  - `detect_result_t detect_note_known(uint32_t hash, int8_t rssi, uint8_t class_id, uint8_t category, uint8_t confidence, uint16_t epoch);` — new KNOWN row → `DETECT_CONFIRM` (sets pending for NVS/LED); existing → `DETECT_KNOWN`.

**Note:** the enum value `DETECT_KNOWN` already exists and is returned by `detect_observe` for "hash already a threat"; reuse it as the "already recorded" signal. Zero-init makes `kind == DETECT_KIND_FOLLOWER` by default, so the existing behavioral path needs no field writes beyond leaving kind at 0. Bump `DETECT_THR_MAGIC` (the struct grew, so old blobs are dropped once on first boot after update — acceptable; behavioral threats re-confirm).

- [ ] **Step 1: Write the failing test** (register `test_detect_known();`)

```c
static void test_detect_known(void)
{
    detect_reset();
    ST_CHECK(detect_note_known(0xAAAA1111, -55, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 3) == DETECT_CONFIRM,
             "known: first hit confirms");
    ST_CHECK(detect_threat_count() == 1, "known: one threat row");
    detect_threat_t t;
    ST_CHECK(detect_threat_at(0, &t) && t.kind == DETECT_KIND_KNOWN, "known: row is KIND_KNOWN");
    ST_CHECK(t.class_id == SIG_CLASS_AIRTAG && t.confidence == 80, "known: class + confidence stored");

    // Re-note updates, does not duplicate.
    ST_CHECK(detect_note_known(0xAAAA1111, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 4) == DETECT_KNOWN,
             "known: repeat updates");
    ST_CHECK(detect_threat_count() == 1, "known: no duplicate");
    detect_threat_at(0, &t); ST_CHECK(t.best_rssi == -40, "known: rssi improved");

    // A behavioral FOLLOWER is not evicted by KNOWN pressure: fill with KNOWN, then confirm a follower,
    // then add more KNOWN — the follower survives.
    detect_reset();
    for (int i = 0; i < DETECT_MAX_THREATS; i++)
        detect_note_known(0xB0000000u + i, -60, SIG_CLASS_TILE, SIG_CAT_TRACKER, 75, 1);
    // Confirm a behavioral follower (3 epochs, >=2 sightings each).
    for (uint16_t e = 1; e <= 3; e++) { detect_observe(0xF0F0F0F0, -50, 0x1234, e); detect_observe(0xF0F0F0F0, -50, 0x1234, e); }
    ST_CHECK(detect_threat_find_kind(0xF0F0F0F0) == DETECT_KIND_FOLLOWER, "known: follower admitted over KNOWN");
    for (int i = 0; i < DETECT_MAX_THREATS; i++)
        detect_note_known(0xC0000000u + i, -60, SIG_CLASS_TILE, SIG_CAT_TRACKER, 75, 5);
    ST_CHECK(detect_threat_find_kind(0xF0F0F0F0) == DETECT_KIND_FOLLOWER, "known: follower not evicted by KNOWN");
}
```

Add a tiny test helper in `churn_selftest.c` (above the test) so the assertion reads cleanly:

```c
static int detect_threat_find_kind(uint32_t hash) {
    for (size_t i = 0; i < detect_threat_count(); i++) {
        detect_threat_t t; if (detect_threat_at(i, &t) && t.hash == hash) return t.kind;
    }
    return -1;
}
```

- [ ] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (`detect_note_known` / fields missing).

- [ ] **Step 3: Extend `detect.h`** — add after the enum and in the struct:

```c
#define DETECT_KIND_FOLLOWER 0
#define DETECT_KIND_KNOWN    1

typedef struct {
    uint32_t hash;
    uint16_t vendor;
    uint8_t  epochs;
    int8_t   best_rssi;
    uint16_t first_epoch;
    uint16_t last_epoch;
    uint8_t  kind;         // DETECT_KIND_*
    uint8_t  class_id;     // sig_class_t (KNOWN only)
    uint8_t  category;     // sig_category_t (KNOWN only)
    uint8_t  confidence;   // 0..100 (KNOWN only)
} detect_threat_t;

detect_result_t detect_note_known(uint32_t hash, int8_t rssi, uint8_t class_id,
                                  uint8_t category, uint8_t confidence, uint16_t epoch);
```

- [ ] **Step 4: Implement in `detect.c`**

Bump the magic (struct grew): `#define DETECT_THR_MAGIC 0x4D394432u   // 'M9D2'`.

Add `detect_note_known` (a KNOWN-row insert/update with follower-priority eviction):

```c
detect_result_t detect_note_known(uint32_t hash, int8_t rssi, uint8_t class_id,
                                  uint8_t category, uint8_t confidence, uint16_t epoch)
{
    if (!s_enabled) return DETECT_NONE;
    detect_threat_t *t = threat_find(hash);
    if (t) {                                   // already recorded: refresh
        if (rssi > t->best_rssi) t->best_rssi = rssi;
        t->last_epoch = epoch;
        return DETECT_KNOWN;
    }
    if (s_threat_n < DETECT_MAX_THREATS) {
        t = &s_threat[s_threat_n++];
    } else {
        // Evict a KNOWN row (oldest) before ever touching a FOLLOWER; if all are FOLLOWERs, drop.
        t = NULL;
        for (size_t i = 0; i < s_threat_n; i++) {
            if (s_threat[i].kind != DETECT_KIND_KNOWN) continue;
            if (!t || s_threat[i].last_epoch < t->last_epoch) t = &s_threat[i];
        }
        if (!t) return DETECT_NONE;            // table full of followers: don't crowd them out
    }
    memset(t, 0, sizeof(*t));
    t->hash = hash; t->best_rssi = rssi; t->first_epoch = epoch; t->last_epoch = epoch;
    t->kind = DETECT_KIND_KNOWN; t->class_id = class_id; t->category = category; t->confidence = confidence;
    s_pending = true; s_pending_threat = *t;
    return DETECT_CONFIRM;
}
```

(The behavioral `promote()` path already zero-inits via `s_threat` reuse; leave `kind` at 0 = FOLLOWER. No other change needed there.)

- [ ] **Step 5: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS. Existing `test_detect_nvs` still passes (magic bump is internal; it saves+loads within one run).

- [ ] **Step 6: Commit**

```bash
git add main/detect.h main/detect.c main/churn_selftest.c
git commit -m "feat(detect): KNOWN threat kind + detect_note_known (follower-priority eviction)"
```

---

### Task 7: status wire carries KNOWN fields

**Files:**
- Modify: `components/simulacra_radar/radar_wire.h` (per-threat element)
- Modify: `main/esp_now_link.c` (`espnow_status_from_webui` copies new fields)
- Modify: `main/webui.h` / wherever `webui_status_t.threats[]` is defined, to carry `kind/class_id/category/confidence` (grep `first_epoch` to find it)
- Test: `main/churn_selftest.c` (`test_espnow_convert` — extend existing)

**Interfaces:**
- Produces: `radar_wire_status_t.threats[i]` gains `uint8_t kind, class_id, category, confidence;` (after `last_epoch`). `espnow_status_from_webui` copies them from the webui snapshot.

- [ ] **Step 1: Extend the failing test** — in the existing `test_espnow_convert`, set the new webui threat fields and assert they survive conversion:

```c
    // (inside test_espnow_convert, after populating an input threat)
    w.threats[0].kind = DETECT_KIND_KNOWN; w.threats[0].class_id = SIG_CLASS_TILE;
    w.threats[0].category = SIG_CAT_TRACKER; w.threats[0].confidence = 75;
    espnow_status_from_webui(&r, &w);
    ST_CHECK(r.threats[0].kind == DETECT_KIND_KNOWN && r.threats[0].class_id == SIG_CLASS_TILE,
             "convert: known fields carried");
    ST_CHECK(r.threats[0].confidence == 75, "convert: confidence carried");
```

- [ ] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (fields missing).

- [ ] **Step 3: Extend the wire struct** — in `radar_wire.h`, the threats element becomes:

```c
    struct __attribute__((packed)) {
        uint32_t hash; uint16_t vendor; uint8_t epochs; int8_t best_rssi;
        uint16_t first_epoch, last_epoch;
        uint8_t kind, class_id, category, confidence;
    } threats[RADAR_MAX_THREATS];
```

Add the same four fields to the `webui_status_t` threat element (find it via grep for `first_epoch` in `main/`), and populate them in `webui_gather_status` from `detect_threat_at` (copy `t.kind/class_id/category/confidence`). In `espnow_status_from_webui` (`main/esp_now_link.c`), add to the per-threat copy loop:

```c
        out->threats[i].kind = in->threats[i].kind;
        out->threats[i].class_id = in->threats[i].class_id;
        out->threats[i].category = in->threats[i].category;
        out->threats[i].confidence = in->threats[i].confidence;
```

- [ ] **Step 4: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS. Also confirm `sizeof(radar_wire_status_t)` stays under 218 (it becomes ~151 B) — the existing `test_radar_wire`/`test_espnow_convert` round-trip covers seal/open.

- [ ] **Step 5: Commit**

```bash
git add components/simulacra_radar/radar_wire.h main/esp_now_link.c main/webui.h main/webui.c main/churn_selftest.c
git commit -m "feat(wire): status frame carries KNOWN threat class/category/confidence"
```

---

### Task 8: decoy RAM signature store

**Files:**
- Create: `main/sig_store.h`, `main/sig_store.c`
- Modify: `main/CMakeLists.txt` (add `sig_store.c` to SRCS)
- Test: `main/churn_selftest.c` (`test_sig_store`)

**Interfaces:**
- Produces:
  - `void sig_store_load_seed(void);`
  - `size_t sig_store_count(void);`
  - `const threat_sig_t *sig_store_db(void);`
  - `uint16_t sig_store_version(void);`
  - `bool sig_store_adopt(const threat_sig_t *recs, size_t n, uint16_t content_version);` — re-gate each; adopt (wholesale swap) iff `content_version >= current` and at least one record survives re-gate; returns true if adopted.

- [ ] **Step 1: Write the failing test** (register `test_sig_store();`)

```c
#include "sig_store.h"

static void test_sig_store(void)
{
    sig_store_load_seed();
    ST_CHECK(sig_store_count() >= 3, "store: seed loaded");
    ST_CHECK(sig_store_version() == sig_seed_version(), "store: seed version");

    // Adopt a newer 1-record DB -> swaps wholesale.
    threat_sig_t one[1] = {0};
    one[0].sig_id = 99; one[0].class_id = SIG_CLASS_TILE; one[0].match_src = SIG_SRC_SVC_DATA;
    one[0].svc_uuid16 = 0xFEED; one[0].pat_len = 0; one[0].confidence = 75;
    ST_CHECK(sig_store_adopt(one, 1, sig_seed_version() + 1), "store: newer version adopted");
    ST_CHECK(sig_store_count() == 1 && sig_store_version() == sig_seed_version()+1, "store: swapped wholesale");

    // Older version ignored.
    ST_CHECK(!sig_store_adopt(one, 1, sig_seed_version()), "store: older version ignored");

    // A batch with a malformed record drops that record on re-gate.
    threat_sig_t two[2] = {0};
    two[0] = one[0]; two[0].sig_id = 5;
    two[1] = one[0]; two[1].sig_id = 6; two[1].pat_len = SIG_PAT_MAX + 5;   // invalid
    ST_CHECK(sig_store_adopt(two, 2, sig_seed_version() + 2), "store: adopt with one bad record");
    ST_CHECK(sig_store_count() == 1, "store: bad record re-gated out");
}
```

- [ ] **Step 2: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build`; FAIL (`sig_store.h` missing).

- [ ] **Step 3: Create `sig_store.h` + `sig_store.c`**

`sig_store.h`:

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "threat_sig.h"
#include "sig_seed.h"

void   sig_store_load_seed(void);
size_t sig_store_count(void);
const threat_sig_t *sig_store_db(void);
uint16_t sig_store_version(void);
bool   sig_store_adopt(const threat_sig_t *recs, size_t n, uint16_t content_version);
```

`sig_store.c`:

```c
#include "sig_store.h"
#include "sig_match.h"      // sig_regate
#include <string.h>

static threat_sig_t s_db[SIG_DB_CAP];
static size_t       s_count;
static uint16_t     s_version;

void sig_store_load_seed(void)
{
    s_count = sig_seed_copy(s_db, SIG_DB_CAP);
    s_version = sig_seed_version();
}
size_t sig_store_count(void)              { return s_count; }
const threat_sig_t *sig_store_db(void)    { return s_db; }
uint16_t sig_store_version(void)          { return s_version; }

bool sig_store_adopt(const threat_sig_t *recs, size_t n, uint16_t content_version)
{
    if (content_version < s_version) return false;      // newest-wins
    threat_sig_t tmp[SIG_DB_CAP]; size_t m = 0;
    for (size_t i = 0; i < n && m < SIG_DB_CAP; i++)
        if (sig_regate(&recs[i])) tmp[m++] = recs[i];   // never trust the wire
    if (m == 0) return false;
    memcpy(s_db, tmp, m * sizeof(threat_sig_t));
    s_count = m; s_version = content_version;
    return true;
}
```

Add `"sig_store.c"` to `main/CMakeLists.txt` SRCS.

- [ ] **Step 4: Run to verify it passes** — `idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`; PASS, +6.

- [ ] **Step 5: Commit**

```bash
git add main/sig_store.h main/sig_store.c main/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(sig): decoy RAM signature store (seed load + re-gated wholesale adopt)"
```

---

### Task 9: decoy observe hook

**Files:**
- Modify: `main/observe.c` (match at the GAP hook), `main/simulacra_main.c` (seed the store at boot)
- Test: build + bench (no new selftest — the matcher/store logic is already covered; this task is transport/wiring)

**Interfaces:**
- Consumes: `sig_match`, `sig_addr_type_from`, `sig_store_db/count`, `detect_note_known`, `observe_hash_mac`.

- [ ] **Step 1: Seed the store at boot** — in `main/simulacra_main.c`, in the decoy branch of `simulacra_task` right after `detect_reset();` (before `coexist_start`), add:

```c
    sig_store_load_seed();   // fingerprint DB: compile-time seed; a Vigil push may replace it
```

Add `#include "sig_store.h"` at the top.

- [ ] **Step 2: Match at the observe hook** — in `main/observe.c` `observe_gap_event`, after the existing `learn_offer(...)` block and before `observe_ingest(...)`, add:

```c
#if SIMULACRA_DETECT
    {
        uint16_t svc16 = (f.num_uuids16 > 0) ? ble_uuid_u16(&f.uuids16[0].u) : 0x0000;
        const uint8_t *svcd = NULL; uint8_t svcd_len = 0;
        if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 2) {   // includes 2-byte uuid + data
            svcd = f.svc_data_uuid16; svcd_len = f.svc_data_uuid16_len;
            if (svc16 == 0) svc16 = (uint16_t)(svcd[0] | (svcd[1] << 8));
        }
        sig_adv_fields_t sf = {
            .company_id = (f.mfg_data && f.mfg_data_len >= 2) ? company : 0xFFFF,
            .svc_uuid16 = svc16,
            .addr_type  = sig_addr_type_from(d->addr.type, d->addr.val),
            .mfg_data   = f.mfg_data, .mfg_len = f.mfg_data_len,
            .svc_data   = svcd,       .svc_len = svcd_len,
        };
        sig_hit_t hit;
        if (sig_match(&sf, sig_store_db(), sig_store_count(), &hit)) {
            detect_note_known(observe_hash_mac(d->addr.val), d->rssi,
                              hit.class_id, hit.category, hit.confidence, s_epoch_for_detect());
            ESP_LOGW(TAG, "sig: KNOWN %s (conf=%u)", sig_class_name(hit.class_id), (unsigned)hit.confidence);
        }
    }
#endif
```

Includes at top of `observe.c`: `#include "sig_match.h"`, `#include "sig_store.h"`, `#include "sig_class_name.h"`, `#include "detect.h"`. For the epoch argument, reuse the same epoch source the M9 tap already feeds to `detect_observe` (grep `detect_observe(` in `observe.c`/`coexist.c`; if detection is driven from `coexist.c`'s `s_report_cb`, call `detect_note_known` there instead, alongside `detect_observe`, passing the same `epoch`). **Wire it wherever `detect_observe` is currently called so both share one epoch counter.**

- [ ] **Step 3: Build the decoy** — `idf.py -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1 -p COM12 build`
Expected: clean build.

- [ ] **Step 4: Bench smoke** — flash; bring a real AirTag near the decoy (or rely on ambient). Read serial:
`& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Port COM12 -Do read -ReadSeconds 30 -Grep 'sig: KNOWN'`
Expected: `sig: KNOWN AirTag (conf=80)` (or SmartTag/Tile) if a tracker is present. (No tracker nearby → no line; that's fine, deferred full check to Task 12.)

- [ ] **Step 5: Commit**

```bash
git add main/observe.c main/simulacra_main.c
git commit -m "feat(sig): decoy matches adverts at the observe hook -> DETECT_KNOWN"
```

---

### Task 10: decoy receives `SIG_SYNC`

**Files:**
- Modify: `main/esp_now_link.c` (handle `RADAR_TYPE_SIG_SYNC`: reassemble + adopt)
- Test: build + bench

**Interfaces:**
- Consumes: `sig_wire_unpack`, `sig_store_adopt`, `sig_chunk_hdr_t`, `RADAR_TYPE_SIG_SYNC`.

- [ ] **Step 1: Add reassembly state + handler** — in `main/esp_now_link.c`, add includes `#include "sig_wire.h"`, `#include "sig_store.h"`, a replay gate `static radar_replay_t s_sig_replay;`, and reassembly state:

```c
static threat_sig_t s_sig_rx[SIG_DB_CAP];
static uint16_t     s_sig_rx_ver;      // version currently being assembled (0 = none)
static uint8_t      s_sig_rx_mask;     // bit i set once chunk i received (chunk_count <= 8)
static uint8_t      s_sig_rx_cnt;      // expected chunk_count
static size_t       s_sig_rx_n;        // records placed so far
```

In `on_recv`, add a branch:

```c
    if (type == RADAR_TYPE_SIG_SYNC) {
        if (!radar_replay_ok(&s_sig_replay, salt, ctr)) return;
        sig_chunk_hdr_t h; threat_sig_t recs[SIG_WIRE_RECS_PER_CHUNK]; uint8_t nr;
        if (sig_wire_unpack(pl, plen, recs, &nr, &h) != 0) return;
        if (h.chunk_count == 0 || h.chunk_count > 8 || h.chunk_index >= h.chunk_count) return;
        if (h.content_version != s_sig_rx_ver) {          // new version -> restart assembly
            s_sig_rx_ver = h.content_version; s_sig_rx_mask = 0; s_sig_rx_cnt = h.chunk_count; s_sig_rx_n = 0;
        }
        size_t off = (size_t)h.chunk_index * SIG_WIRE_RECS_PER_CHUNK;
        for (uint8_t i = 0; i < nr && off + i < SIG_DB_CAP; i++) s_sig_rx[off + i] = recs[i];
        s_sig_rx_mask |= (uint8_t)(1u << h.chunk_index);
        if (off + nr > s_sig_rx_n) s_sig_rx_n = off + nr;
        uint8_t full = (uint8_t)((1u << s_sig_rx_cnt) - 1);
        if ((s_sig_rx_mask & full) == full) {             // all chunks in
            if (sig_store_adopt(s_sig_rx, s_sig_rx_n, s_sig_rx_ver))
                ESP_LOGW(ETAG, "sig: adopted DB v%u (%u sigs)", (unsigned)s_sig_rx_ver, (unsigned)sig_store_count());
            s_sig_rx_ver = 0; s_sig_rx_mask = 0;          // reset for the next announce
        }
        return;
    }
```

- [ ] **Step 2: Build the decoy** — `idf.py -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1 -p COM12 build`; clean.

- [ ] **Step 3: Bench (after Task 11 flashes Vigil)** — deferred to Task 12; the send side must exist first.

- [ ] **Step 4: Commit**

```bash
git add main/esp_now_link.c
git commit -m "feat(sig): decoy reassembles SIG_SYNC chunks + re-gated adopt"
```

---

### Task 11: Vigil custody + broadcast + display

**Files:**
- Modify: `cyd/main/cyd_main.c` (load/seal card DB, broadcast `SIG_SYNC`), `components/simulacra_radar/radar_render.c` (label + color KNOWN threats)
- Test: build + bench

**Interfaces:**
- Consumes: `sig_db_*`, `sig_seed_*`, `sig_wire_pack`, `RADAR_TYPE_SIG_SYNC`, `radar_wire_seal`, `sig_class_name`.

- [ ] **Step 1: Vigil signature DB state + load/seal** — in `cyd/main/cyd_main.c`, near the learn DB state, add:

```c
#include "sig_db.h"
#include "sig_wire.h"
#include "sig_seed.h"
#define SIG_DB_PATH SD_MOUNT_POINT "/simulacra/threat_sig.db"
#define SIG_DB_TMP  SD_MOUNT_POINT "/simulacra/threat_sig.tmp"
static threat_sig_t s_sigdb[SIG_DB_CAP]; static size_t s_sigdb_n; static uint16_t s_sigdb_ver;
static uint8_t      s_sigdb_key[32];

static void sig_db_save_card(void) {
    if (!s_sd_ok || s_sigdb_n == 0) return;
    static uint8_t blob[sizeof(sig_db_hdr_t)+SIG_DB_CAP*sizeof(threat_sig_t)]; size_t blen;
    if (sig_db_seal(blob,&blen,s_sigdb,(uint16_t)s_sigdb_n,s_sigdb_ver,s_sigdb_key)!=0) return;
    FILE *f=fopen(SIG_DB_TMP,"wb"); if(!f) return;
    size_t w=fwrite(blob,1,blen,f); fclose(f);
    if(w!=blen){ remove(SIG_DB_TMP); return; }
    remove(SIG_DB_PATH); if(rename(SIG_DB_TMP,SIG_DB_PATH)==0) ESP_LOGW(TAG,"sigdb: saved v%u (%u sigs)",(unsigned)s_sigdb_ver,(unsigned)s_sigdb_n);
}
static void sig_db_init(void) {
    sig_db_derive_key(SIMULACRA_ESPNOW_KEY, s_sigdb_key);
    s_sigdb_n = sig_seed_copy(s_sigdb, SIG_DB_CAP); s_sigdb_ver = sig_seed_version();   // baseline = seed
    if (s_sd_ok) {
        FILE *f=fopen(SIG_DB_PATH,"rb");
        if (f) {
            static uint8_t blob[sizeof(sig_db_hdr_t)+SIG_DB_CAP*sizeof(threat_sig_t)];
            fseek(f,0,SEEK_END); long fsz=ftell(f); fseek(f,0,SEEK_SET);
            size_t n = (fsz>0 && (size_t)fsz<=sizeof blob) ? fread(blob,1,(size_t)fsz,f) : 0; fclose(f);
            threat_sig_t tmp[SIG_DB_CAP]; uint16_t cnt=0, ver=0;
            if (n && sig_db_open(blob,n,tmp,&cnt,&ver,s_sigdb_key)==0 && ver >= s_sigdb_ver && cnt<=SIG_DB_CAP) {
                memcpy(s_sigdb,tmp,cnt*sizeof(threat_sig_t)); s_sigdb_n=cnt; s_sigdb_ver=ver;
                ESP_LOGW(TAG,"sigdb: loaded v%u (%u sigs) from card",(unsigned)ver,(unsigned)cnt);
            }
        }
    }
    sig_db_save_card();                       // self-populate a fresh/older card with the seed
}
```

Call `sig_db_init();` in `app_main` right after `learn_db_load();`.

- [ ] **Step 2: Broadcast `SIG_SYNC` every 60 s** — add:

```c
static void broadcast_sig_db(void) {
    if (s_sigdb_n == 0) return;
    uint8_t chunks = (uint8_t)((s_sigdb_n + SIG_WIRE_RECS_PER_CHUNK - 1) / SIG_WIRE_RECS_PER_CHUNK);
    for (uint8_t ci = 0; ci < chunks; ci++) {
        size_t off = (size_t)ci * SIG_WIRE_RECS_PER_CHUNK;
        uint8_t nrec = (uint8_t)((s_sigdb_n - off < SIG_WIRE_RECS_PER_CHUNK) ? (s_sigdb_n - off) : SIG_WIRE_RECS_PER_CHUNK);
        uint8_t pl[RADAR_FRAME_MAX]; size_t plen;
        if (sig_wire_pack(pl,&plen,&s_sigdb[off],nrec,s_sigdb_ver,ci,chunks)!=0) continue;
        uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
        if (radar_wire_seal(frame,&flen,RADAR_TYPE_SIG_SYNC,pl,plen,SIMULACRA_ESPNOW_KEY,s_salt,++s_ctr)==0)
            esp_now_send(BCAST,frame,flen);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG,"sig: broadcast v%u (%u sigs, %u chunks)",(unsigned)s_sigdb_ver,(unsigned)s_sigdb_n,chunks);
}
```

In the main loop, alongside the learn `broadcast_library` timer, add a 60 s timer:

```c
        static uint32_t last_sig = 0;
        if (now - last_sig > 60000) { last_sig = now; broadcast_sig_db(); }
```

- [ ] **Step 3: Label + color KNOWN threats** — in `components/simulacra_radar/radar_render.c`, add `#include "sig_class_name.h"`. In `draw_detail`, render KNOWN rows distinctly:

```c
        if (st->threats[i].kind == DETECT_KIND_KNOWN) {
            const char *q = st->threats[i].confidence >= 80 ? "likely" : "possible";
            snprintf(r, sizeof r, "%s (%s) %ddB", sig_class_name(st->threats[i].class_id), q,
                     (int)st->threats[i].best_rssi);
            radar_gfx_text(g, 6, 30+i*18, r, 0xFD20);   // amber = KNOWN device
        } else {
            /* existing follower row (unchanged) */
        }
```

Add `#include "detect.h"` guard is not needed in the shared component; instead define `#define DETECT_KIND_KNOWN 1` locally in `radar_render.c` or read it from `radar_wire.h`. **Put `DETECT_KIND_FOLLOWER/KNOWN` in `radar_wire.h`** (shared) so both `detect.h` and `radar_render.c` reference one definition — update Task 6/Task 7 to `#include`/reference it. In `draw_radar`, give KNOWN blips a distinct fill color (e.g. `0xFD20` amber) vs. the follower `threat_color`.

- [ ] **Step 4: Build Vigil** — from `cyd/`: `idf.py build`; clean, no DRAM overflow.

- [ ] **Step 5: Commit**

```bash
git add cyd/main/cyd_main.c components/simulacra_radar/radar_render.c components/simulacra_radar/radar_wire.h main/detect.h
git commit -m "feat(vigil): signature DB custody on SD + SIG_SYNC broadcast + KNOWN display"
```

---

### Task 12: two-board bench verification + finish

**Files:** none (bench).

- [ ] **Step 1: Flash both** — Vigil (from `cyd/`): `idf.py -p COM10 flash`. Decoy: `idf.py -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1 -p COM12 flash`.

- [ ] **Step 2: DB distribution** — read the decoy: `build_flash_read.ps1 -Target c5 -Port COM12 -Do read -ReadSeconds 75 -Grep 'sig: adopted|sig: KNOWN'`. Expect `sig: adopted DB v1 (3 sigs)` within ~60 s (Vigil's first `SIG_SYNC`). Read Vigil (COM10): expect `sigdb: loaded`/`sigdb: saved` and `sig: broadcast v1 (3 sigs…)`.

- [ ] **Step 3: Live match** — bring a real AirTag (or SmartTag/Tile) near the decoy. Expect decoy `sig: KNOWN AirTag (conf=80)`, and on the CYD DETAIL page a `AirTag (likely)` row (verify via CYD `status rx` serial if the screen isn't visible). **Confirm the seed pattern bytes against this live capture** (Task 5 note); adjust `sig_seed.c` if a real tracker doesn't match, then re-flash.

- [ ] **Step 4: Regression** — flip to a selftest build (`idf.py -DCHURN_SELFTEST=1 -p COM12 build flash monitor`) and confirm `SELFTEST result: PASS` with the new total (all sig tests green). Revert to the display-paired build after.

- [ ] **Step 5: Finish** — mark checkboxes; then use superpowers:finishing-a-development-branch (verify selftest PASS, offer merge options; established pattern = `--no-ff` merge to local `main`).

---

## Self-Review

**Spec coverage:** signature record (T1) · matcher (T1) · re-gate (T1/T2) · encrypted SD DB (T3) · ESP-NOW framing (T4) · seed set + honesty confidence (T5) · detect KNOWN kind + follower-priority eviction (T6) · status wire (T7) · decoy RAM store + adopt (T8) · observe hook (T9) · decoy receive (T10) · Vigil custody/broadcast/display (T11) · bench + honesty confirm (T12). Graceful degradation = seed load in T8/T9 with no Vigil. All spec sections mapped.

**Type consistency:** `threat_sig_t`, `sig_adv_fields_t`, `sig_hit_t`, `sig_match`, `sig_regate`, `sig_addr_type_from`, `sig_db_seal/open` (with `content_version`), `sig_wire_pack/unpack`, `sig_seed_copy/version`, `detect_note_known`, `DETECT_KIND_*` (single definition in `radar_wire.h` per T11 note), `sig_store_*` — all consistent across tasks.

**Known follow-ups folded in:** `DETECT_KIND_*` lives in `radar_wire.h` (shared) to avoid a `detect.h` dependency from the shared renderer (noted in T11, referenced by T6/T7). Seed byte values are explicitly flagged for live confirmation in T5 + T12.
