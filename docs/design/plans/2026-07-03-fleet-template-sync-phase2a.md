# Fleet Template Sync — Phase 2a (Shared Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the host-testable foundation for fleet template sharing — relocate the learned-template primitives into the shared `simulacra_radar` component and add the wire serialization, idempotent merge, and re-gate that both the decoy and Vigil will use.

**Architecture:** Move `law3`, `learned_template_t`, and `learn_shape_hash` from the decoy-only `main/` into the shared component so Vigil (CYD) can re-gate and merge without duplicating decoy code. Add `learn_wire.{c,h}` (new, in the component) carrying the `LEARN_OFFER`/`LEARN_SYNC` record serialization + chunk framing, the idempotent `learn_merge`, and the `learn_regate` safety check. All logic is verified in the decoy on-target selftest; the CYD project is build-checked to confirm the shared component compiles for Vigil.

**Tech Stack:** C (ESP-IDF), the existing `radar_wire` AES-GCM framing, on-target selftest harness (`churn_selftest.c`, `ST_CHECK`).

## Global Constraints

- **Law 3 (absolute):** no forbidden subtype ever emitted or stored (Apple pop-up `0x07`/`0x0F`/`0x12`, Swift Pair `0x0006`, Fast Pair svc-data `0xFE2C`). Every received record is re-gated.
- **Never trust the wire.** A record arriving over ESP-NOW is re-run through `learn_regate` (law3 + budget + shape_hash recompute) before it is merged.
- **`learned_template_t` is a flat, `__attribute__((packed))` POD** so its byte layout is deterministic across the decoy (RISC-V C5/C6) and Vigil (Xtensa classic ESP32) for memcpy-based wire serialization.
- **Wire frames** reuse `radar_wire` (`[magic|ver|type|nonce(12)|ct|tag(16)]`, AES-256-GCM, `RADAR_FRAME_MAX 250`). Payload budget after overhead = `250 - 4 - 12 - 16 = 218` bytes.
- **Merge is idempotent** — keyed by `shape_hash`; re-receiving a record is harmless.
- **Two ESP-IDF projects share `components/simulacra_radar/`:** the decoy (`/`, IDF 5.5 for C5) and Vigil (`cyd/`, IDF 5.4 for classic ESP32). A component source change must compile for both.
- **Self-tests run on-target** under `CHURN_SELFTEST=1`; a call to an unimplemented function fails as a build/link error (the TDD "red").

### Build / flash / read commands (every task uses these)

Decoy selftest on the C5 (Ward, COM12):

```powershell
$env:PATH = "C:\Program Files\Python312;C:\Program Files\Python312\Scripts;$env:PATH"
$env:IDF_PATH = "~\esp\v5.5\esp-idf"; . "$env:IDF_PATH\export.ps1"
Set-Location ~\Downloads\splinter
idf.py -B build.c5 -DCHURN_SELFTEST=1 build            # red: link error on unimplemented fn
idf.py -B build.c5 -DCHURN_SELFTEST=1 -p COM12 flash
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM12 --seconds 8 --reset yes 2>&1 | Select-String 'FAIL:|SELFTEST:'
```
Expected green: `SELFTEST: PASS (N/N)`.

CYD (Vigil) build-check — confirms the shared component compiles for classic ESP32 (IDF 5.4). **Use a separate shell** (different IDF_PATH):

```powershell
$env:PATH = "C:\Program Files\Python312;C:\Program Files\Python312\Scripts;$env:PATH"
$env:IDF_PATH = "~\esp\v5.4\esp-idf"; . "$env:IDF_PATH\export.ps1"
Set-Location ~\Downloads\splinter\cyd
# if build/ is on the wrong target: idf.py set-target esp32
idf.py build          # expect "Project build complete"
```

---

### Task 1: Relocate `law3` into the shared component

Move the Law-3 guard from `main/` into `components/simulacra_radar/` so Vigil can re-gate received records with the same implementation.

**Files:**
- Move: `main/law3.h` → `components/simulacra_radar/law3.h`
- Move: `main/law3.c` → `components/simulacra_radar/law3.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `law3.c` to SRCS)
- Modify: `main/CMakeLists.txt` (remove `law3.c` from SRCS)

**Interfaces:**
- Produces (unchanged signatures, new location): `has_apple_popup_subtype`, `law3_forbidden` — now includable by both projects via the component's `INCLUDE_DIRS "."`.

- [ ] **Step 1: Move the files** (git mv preserves history):

```bash
git mv main/law3.h components/simulacra_radar/law3.h
git mv main/law3.c components/simulacra_radar/law3.c
```

- [ ] **Step 2: Add `law3.c` to the component SRCS** — edit `components/simulacra_radar/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "radar_geom.c" "radar_ui.c" "radar_wire.c" "radar_gfx.c" "radar_render.c" "law3.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls
)
```

- [ ] **Step 3: Remove `law3.c` from the decoy SRCS** — in `main/CMakeLists.txt`, delete the `"law3.c"` token from the `SRCS` list (leave `"learn.c"`). The `#include "law3.h"` sites in `main/` now resolve via the component include dir.

- [ ] **Step 4: Build + flash + read the decoy selftest** — expect `SELFTEST: PASS` unchanged (all `law3:` and `strip:` checks still pass; the guard is byte-identical, only relocated).

- [ ] **Step 5: Build-check the CYD project** (separate IDF 5.4 shell, commands above) — expect `Project build complete`. This proves `law3.c` compiles for classic ESP32 in the shared component.

- [ ] **Step 6: Commit**

```bash
git add main/CMakeLists.txt components/simulacra_radar/CMakeLists.txt components/simulacra_radar/law3.h components/simulacra_radar/law3.c
git commit -m "refactor(learn): relocate Law-3 guard into shared simulacra_radar component"
```

---

### Task 2: Relocate `learned_template_t` + `learn_shape_hash` into the shared component

Move the record struct and its structure-hash into the component (as a packed POD) so Vigil shares the exact type and hashing.

**Files:**
- Create: `components/simulacra_radar/learn_record.h` (the struct + `learn_shape_hash` declaration)
- Create: `components/simulacra_radar/learn_wire.c` (holds `learn_shape_hash`; more added in later tasks)
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `learn_wire.c`)
- Modify: `main/learn.h` (remove the struct + `learn_shape_hash` decl; `#include "learn_record.h"`)
- Modify: `main/learn.c` (remove the `learn_shape_hash` definition; it now lives in the component)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `learned_template_t` (packed) in `learn_record.h`.
  - `uint32_t learn_shape_hash(const learned_template_t *t);` (moved to `learn_wire.c`).

- [ ] **Step 1: Create `components/simulacra_radar/learn_record.h`:**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// A learned archetype: identity-stripped AD skeleton + metadata. PACKED so the
// byte layout is deterministic across the decoy (RISC-V) and Vigil (Xtensa) for
// memcpy-based wire serialization. Shared by main/learn.c and the Vigil librarian.
typedef struct __attribute__((packed)) {
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

// FNV-1a over family + company_id + svc_uuid + the AD-type/length sequence
// (NOT value bytes) -> dedup / merge key.
uint32_t learn_shape_hash(const learned_template_t *t);
```

- [ ] **Step 2: Create `components/simulacra_radar/learn_wire.c`** with `learn_shape_hash` moved verbatim from `main/learn.c`:

```c
#include "learn_record.h"

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
```

- [ ] **Step 3: Add `learn_wire.c` to the component SRCS** — `components/simulacra_radar/CMakeLists.txt`:

```cmake
    SRCS "radar_geom.c" "radar_ui.c" "radar_wire.c" "radar_gfx.c" "radar_render.c" "law3.c" "learn_wire.c"
```

- [ ] **Step 4: Edit `main/learn.h`** — replace the inline struct + `learn_shape_hash` decl with an include. Delete the whole `typedef struct { ... } learned_template_t;` block and the `uint32_t learn_shape_hash(...)` line, and add near the top (after the other includes):

```c
#include "learn_record.h"   // learned_template_t + learn_shape_hash (shared component)
```

Keep everything else in `main/learn.h` (tunables `LEARN_CAP`/`LEARN_MIN_SIGHTINGS`/etc., store/pipeline/NVS API).

- [ ] **Step 5: Edit `main/learn.c`** — delete the `learn_shape_hash` function definition (the whole `uint32_t learn_shape_hash(...) { ... }` block); it now lives in `learn_wire.c`. All call sites remain valid.

- [ ] **Step 6: Build + flash + read the decoy selftest** — expect `SELFTEST: PASS` unchanged (hash/render/store/nvs tests still pass; the struct is now packed but round-trips identically, and the NVS blob simply reloads under its versioned check).

- [ ] **Step 7: Build-check the CYD project** — expect `Project build complete` (the component now carries `learn_record.h` + `learn_wire.c`).

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/learn_record.h components/simulacra_radar/learn_wire.c components/simulacra_radar/CMakeLists.txt main/learn.h main/learn.c
git commit -m "refactor(learn): share learned_template_t (packed) + shape_hash via component"
```

---

### Task 3: Shared idempotent `learn_merge`

Extract the store dedup/reinforce/evict logic into a shared function operating on a caller-supplied array, so both the decoy store and the Vigil librarian merge identically. Rewire the decoy's `learn_store_add` to it.

**Files:**
- Modify: `components/simulacra_radar/learn_wire.c` (add `learn_merge` + static helpers)
- Create: `components/simulacra_radar/learn_wire.h` (declare `learn_merge`)
- Modify: `main/learn.c` (`learn_store_add` becomes a thin wrapper; remove the now-shared `find_shape`/`weakest`)

**Interfaces:**
- Consumes: `learn_shape_hash` (Task 2).
- Produces:
  - `bool learn_merge(learned_template_t *store, size_t *count, size_t cap, const learned_template_t *rec, uint16_t sweep_no);` — dedup by `shape_hash`: reinforce (saturating `reinforce_count++`, refresh `last_seen_sweep`, widen interval band) if present; else insert, evicting the weakest (lowest `reinforce_count`, tie oldest `last_seen_sweep`) when full. Returns false only if `rec` is weaker than every entry when full.

- [ ] **Step 1: Create `components/simulacra_radar/learn_wire.h`:**

```c
#pragma once
#include "learn_record.h"

// Idempotent merge of one record into a (store,count,cap) library, keyed by shape_hash.
// Reinforce if present (saturating count, refresh last_seen, widen interval band); else
// insert, evicting the weakest when full. Returns false iff full and rec is the weakest.
bool learn_merge(learned_template_t *store, size_t *count, size_t cap,
                 const learned_template_t *rec, uint16_t sweep_no);
```

- [ ] **Step 2: Add to `components/simulacra_radar/learn_wire.c`** (after `learn_shape_hash`, add `#include "learn_wire.h"` at the top):

```c
static int lw_find(const learned_template_t *store, size_t count, uint32_t hash)
{
    for (size_t i = 0; i < count; i++) if (store[i].shape_hash == hash) return (int)i;
    return -1;
}
static size_t lw_weakest(const learned_template_t *store, size_t count)
{
    size_t w = 0;
    for (size_t i = 1; i < count; i++)
        if (store[i].reinforce_count < store[w].reinforce_count ||
            (store[i].reinforce_count == store[w].reinforce_count &&
             store[i].last_seen_sweep < store[w].last_seen_sweep)) w = i;
    return w;
}

bool learn_merge(learned_template_t *store, size_t *count, size_t cap,
                 const learned_template_t *rec, uint16_t sweep_no)
{
    int idx = lw_find(store, *count, rec->shape_hash);
    if (idx >= 0) {
        learned_template_t *e = &store[idx];
        if (e->reinforce_count < 0xFFFF) e->reinforce_count++;
        e->last_seen_sweep = sweep_no;
        if (rec->itvl_min_ms < e->itvl_min_ms) e->itvl_min_ms = rec->itvl_min_ms;
        if (rec->itvl_max_ms > e->itvl_max_ms) e->itvl_max_ms = rec->itvl_max_ms;
        return true;
    }
    if (*count < cap) {
        store[*count] = *rec; store[*count].last_seen_sweep = sweep_no; (*count)++;
        return true;
    }
    size_t w = lw_weakest(store, *count);
    if (rec->reinforce_count < store[w].reinforce_count) return false;
    store[w] = *rec; store[w].last_seen_sweep = sweep_no;
    return true;
}
```

- [ ] **Step 3: Rewire `main/learn.c`** — replace the body of `learn_store_add` and delete the now-duplicated static `find_shape`/`weakest`. Add `#include "learn_wire.h"` near the top. New body:

```c
bool learn_store_add(const learned_template_t *t, uint16_t sweep_no)
{
    return learn_merge(s_store, &s_count, LEARN_CAP, t, sweep_no);
}
```

Delete the `static int find_shape(...)` and `static size_t weakest(...)` definitions from `main/learn.c` (they are unused now; `learn_age_out` does not use them). Leave `learn_age_out` unchanged.

- [ ] **Step 4: Build + flash + read the decoy selftest** — expect `SELFTEST: PASS` unchanged (the `store:` dedup/reinforce/eviction checks now exercise `learn_merge`).

- [ ] **Step 5: Build-check the CYD project** — expect `Project build complete`.

- [ ] **Step 6: Commit**

```bash
git add components/simulacra_radar/learn_wire.h components/simulacra_radar/learn_wire.c main/learn.c
git commit -m "refactor(learn): shared idempotent learn_merge; decoy store wraps it"
```

---

### Task 4: Record serialization + chunk framing (`LEARN_OFFER`/`LEARN_SYNC`)

Pack/unpack arrays of learned records into `radar_wire` payload-sized chunks, so a library can be shipped across many frames and reassembled by idempotent merge.

**Files:**
- Modify: `components/simulacra_radar/learn_wire.h` (types + pack/unpack)
- Modify: `components/simulacra_radar/learn_wire.c`
- Modify: `main/churn_selftest.c` (`test_learn_wire()`)

**Interfaces:**
- Consumes: `learned_template_t` (Task 2), `learn_merge` (Task 3).
- Produces:
  - `#define RADAR_TYPE_LEARN_OFFER 3`, `#define RADAR_TYPE_LEARN_SYNC 4`
  - `#define LEARN_WIRE_RECS_PER_CHUNK 3`
  - `learn_chunk_hdr_t` (packed): `{ uint16_t lib_version; uint8_t chunk_index; uint8_t chunk_count; uint8_t rec_count; }`
  - `int learn_wire_pack(uint8_t *payload, size_t *plen, const learned_template_t *recs, uint8_t nrecs, uint16_t lib_version, uint8_t chunk_index, uint8_t chunk_count);` — returns 0; `*plen <= 218`.
  - `int learn_wire_unpack(const uint8_t *payload, size_t plen, learned_template_t *recs, uint8_t *nrecs, learn_chunk_hdr_t *hdr);` — returns 0 on a well-formed chunk, <0 otherwise.

- [ ] **Step 1: Write the failing test** in `churn_selftest.c` (`#include "learn_wire.h"` near the other learn include; call `test_learn_wire();` after `test_learn_generate();`):

```c
#include "learn_wire.h"

static void test_learn_wire(void)
{
    // Build a 4-record set (distinct shapes).
    learned_template_t set[4];
    for (int i = 0; i < 4; i++) { mk_shape(&set[i], (uint16_t)(0x0070 + i)); }

    // Pack as two chunks: [0..3) and [3..4).
    uint8_t p0[218], p1[218]; size_t l0, l1;
    ST_CHECK(learn_wire_pack(p0, &l0, &set[0], 3, 1, 0, 2, 100) == 0 && l0 <= 218, "wire: chunk0 packs");
    ST_CHECK(learn_wire_pack(p1, &l1, &set[3], 1, 1, 1, 2, 100) == 0 && l1 <= 218, "wire: chunk1 packs");

    // Reassemble via merge into a fresh store; out-of-order + a duplicate chunk0 must be harmless.
    learned_template_t store[8]; size_t cnt = 0; learn_chunk_hdr_t h;
    learned_template_t rx[LEARN_WIRE_RECS_PER_CHUNK]; uint8_t n;
    ST_CHECK(learn_wire_unpack(p1, l1, rx, &n, &h) == 0 && n == 1 && h.chunk_count == 2, "wire: unpack c1");
    for (uint8_t i = 0; i < n; i++) learn_merge(store, &cnt, 8, &rx[i], 1);
    ST_CHECK(learn_wire_unpack(p0, l0, rx, &n, &h) == 0 && n == 3, "wire: unpack c0");
    for (uint8_t i = 0; i < n; i++) learn_merge(store, &cnt, 8, &rx[i], 1);
    ST_CHECK(learn_wire_unpack(p0, l0, rx, &n, &h) == 0, "wire: unpack dup c0");
    for (uint8_t i = 0; i < n; i++) learn_merge(store, &cnt, 8, &rx[i], 1);   // idempotent
    ST_CHECK(cnt == 4, "wire: reassembly complete + idempotent (4 records)");

    // Packing more than the chunk capacity is rejected.
    ST_CHECK(learn_wire_pack(p0, &l0, set, LEARN_WIRE_RECS_PER_CHUNK + 1, 1, 0, 1, 100) != 0,
             "wire: over-capacity pack rejected");
}
```

- [ ] **Step 2: Build to verify it fails** — `undefined reference to 'learn_wire_pack'`.

- [ ] **Step 3: Add types + declarations to `learn_wire.h`:**

```c
#define RADAR_TYPE_LEARN_OFFER 3
#define RADAR_TYPE_LEARN_SYNC  4
#define LEARN_WIRE_RECS_PER_CHUNK 3    // (218 - hdr) / sizeof(record) with margin

typedef struct __attribute__((packed)) {
    uint16_t lib_version;
    uint8_t  chunk_index;
    uint8_t  chunk_count;
    uint8_t  rec_count;
} learn_chunk_hdr_t;

int learn_wire_pack(uint8_t *payload, size_t *plen, const learned_template_t *recs, uint8_t nrecs,
                    uint16_t lib_version, uint8_t chunk_index, uint8_t chunk_count);
int learn_wire_unpack(const uint8_t *payload, size_t plen, learned_template_t *recs,
                      uint8_t *nrecs, learn_chunk_hdr_t *hdr);
```

- [ ] **Step 4: Implement in `learn_wire.c`** (add `#include <string.h>` at the top if absent):

```c
int learn_wire_pack(uint8_t *payload, size_t *plen, const learned_template_t *recs, uint8_t nrecs,
                    uint16_t lib_version, uint8_t chunk_index, uint8_t chunk_count)
{
    if (nrecs > LEARN_WIRE_RECS_PER_CHUNK) return -1;
    learn_chunk_hdr_t h = { lib_version, chunk_index, chunk_count, nrecs };
    memcpy(payload, &h, sizeof h);
    memcpy(payload + sizeof h, recs, (size_t)nrecs * sizeof(learned_template_t));
    *plen = sizeof h + (size_t)nrecs * sizeof(learned_template_t);
    return 0;
}

int learn_wire_unpack(const uint8_t *payload, size_t plen, learned_template_t *recs,
                      uint8_t *nrecs, learn_chunk_hdr_t *hdr)
{
    if (plen < sizeof(learn_chunk_hdr_t)) return -1;
    memcpy(hdr, payload, sizeof *hdr);
    if (hdr->rec_count > LEARN_WIRE_RECS_PER_CHUNK) return -1;
    size_t need = sizeof(learn_chunk_hdr_t) + (size_t)hdr->rec_count * sizeof(learned_template_t);
    if (plen < need) return -1;
    memcpy(recs, payload + sizeof(learn_chunk_hdr_t),
           (size_t)hdr->rec_count * sizeof(learned_template_t));
    *nrecs = hdr->rec_count;
    return 0;
}
```

> Sizing check: `sizeof(learned_template_t)` packed = 55 B; `sizeof(learn_chunk_hdr_t)` = 5 B. `5 + 3*55 = 170 <= 218`. A 128-record library is `ceil(128/3) = 43` chunks.

- [ ] **Step 5: Build + flash + read** — expect `SELFTEST: PASS` with the `wire:` checks.

- [ ] **Step 6: Build-check the CYD project** — expect `Project build complete`.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/learn_wire.h components/simulacra_radar/learn_wire.c main/churn_selftest.c
git commit -m "feat(learn): LEARN_OFFER/LEARN_SYNC record serialization + chunk framing"
```

---

### Task 5: Re-gate helper (`learn_regate`)

Validate a record received over the wire before merging — the "never trust the wire" gate that runs at both Vigil and the decoy in Phase 2b.

**Files:**
- Modify: `components/simulacra_radar/learn_wire.h` (declare `learn_regate`)
- Modify: `components/simulacra_radar/learn_wire.c`
- Modify: `main/churn_selftest.c` (`test_learn_regate()`)

**Interfaces:**
- Consumes: `law3_forbidden` (Task 1), `learn_shape_hash` (Task 2).
- Produces:
  - `bool learn_regate(const learned_template_t *rec);` — true iff `1 <= ad_len <= 31`, `!law3_forbidden(rec->ad, rec->ad_len)`, and `learn_shape_hash(rec) == rec->shape_hash`. A record failing any check is dropped, never merged.

- [ ] **Step 1: Write the failing test** (`test_learn_regate();`):

```c
static void test_learn_regate(void)
{
    learned_template_t ok; mk_shape(&ok, 0x0075);
    ST_CHECK(learn_regate(&ok), "regate: clean record accepted");

    // Tampered: inject an Apple nearby-action subtype into the skeleton.
    learned_template_t evil = ok;
    evil.ad_len = 9;
    uint8_t bad[9] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    memcpy(evil.ad, bad, 9);
    evil.shape_hash = learn_shape_hash(&evil);   // even with a valid hash, law3 must reject
    ST_CHECK(!learn_regate(&evil), "regate: forbidden subtype rejected");

    // Oversized length rejected.
    learned_template_t big = ok; big.ad_len = 32;
    ST_CHECK(!learn_regate(&big), "regate: over-budget ad_len rejected");

    // shape_hash mismatch (corrupt / spoofed) rejected.
    learned_template_t liar = ok; liar.shape_hash ^= 0xFFFFFFFFu;
    ST_CHECK(!learn_regate(&liar), "regate: shape_hash mismatch rejected");
}
```

- [ ] **Step 2: Build to verify it fails** — `undefined reference to 'learn_regate'`.

- [ ] **Step 3: Declare in `learn_wire.h`:**

```c
#include "law3.h"
// Re-validate a wire-received record before merging (never trust the wire):
// budget + Law-3 + shape_hash recompute must all hold.
bool learn_regate(const learned_template_t *rec);
```

- [ ] **Step 4: Implement in `learn_wire.c`:**

```c
bool learn_regate(const learned_template_t *rec)
{
    if (rec->ad_len == 0 || rec->ad_len > 31) return false;
    if (law3_forbidden(rec->ad, rec->ad_len)) return false;
    if (learn_shape_hash(rec) != rec->shape_hash) return false;
    return true;
}
```

- [ ] **Step 5: Build + flash + read** — expect `SELFTEST: PASS` with the `regate:` checks.

- [ ] **Step 6: Build-check the CYD project** — expect `Project build complete`.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/learn_wire.h components/simulacra_radar/learn_wire.c main/churn_selftest.c
git commit -m "feat(learn): learn_regate — never-trust-the-wire re-validation"
```

---

## Self-Review

**Spec coverage (this is the foundation subset of `2026-07-03-fleet-template-sync-design.md`):**
- Wire protocol frame types + chunk framing (spec §Wire protocol) → Task 4.
- Idempotent merge by `shape_hash`, saturating reinforce, interval union (spec §Merge semantics) → Task 3.
- Re-gate every received template (spec §Load-bearing principle 2) → Task 5.
- Shared `learned_template_t` as a flat POD (spec §Wire protocol frame math) → Task 2 (packed).
- **Deferred to Phase 2b (out of scope here, tracked):** SD mount + encrypted `learn.db` (spec §SD storage), Vigil RX/merge/persist + TX top-N and decoy dirty-tracking/offer/receive wiring (spec §Sync model, §Cadence), Vigil UI page (spec §Vigil UI), on-target 3-board soak (spec §Testing). Phase 2b builds directly on Tasks 1–5.

**Placeholder scan:** every code step is complete; commands are explicit. No TBD/TODO.

**Type consistency:** `learned_template_t` defined once (Task 2, packed) and used identically in Tasks 3–5. `learn_merge(store,&count,cap,rec,sweep_no)` signature matches between Task 3 definition and Task 4 test usage. `learn_chunk_hdr_t` fields (`lib_version/chunk_index/chunk_count/rec_count`) consistent between Task 4 definition and test. `learn_shape_hash`/`law3_forbidden` signatures unchanged by relocation (Tasks 1–2). `LEARN_WIRE_RECS_PER_CHUNK` used consistently in pack/unpack bounds and the over-capacity test.

**Note for the implementer:** Tasks 1–3 are refactors verified by the *existing* selftest staying green (plus the CYD build-check); Tasks 4–5 add new `wire:`/`regate:` checks. Every task must keep both projects building.
