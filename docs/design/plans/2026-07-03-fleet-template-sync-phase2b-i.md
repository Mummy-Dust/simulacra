# Fleet Template Sync — Phase 2b-i (ESP-NOW Sync Core, RAM) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the fleet actually share learned templates over ESP-NOW — a decoy offers its library, Vigil merges it into a RAM library and broadcasts the pool back, and every decoy merges what it receives. No SD yet (that is Phase 2b-ii).

**Architecture:** Reuse the Phase 2a `learn_wire` primitives (pack/unpack, regate, merge) and the existing `radar_wire` AES-GCM transport. Add a max-reinforce wire merge, expose a snapshot/ingest API on the decoy's learn store, wire `LEARN_OFFER` send + `LEARN_SYNC` receive into the decoy's `esp_now_link`, and give Vigil a RAM library that receives offers, re-gates + merges them, and broadcasts the top-N back.

**Tech Stack:** C (ESP-IDF), `radar_wire`/`esp_now` transport, the shared `simulacra_radar` component, on-target selftest + two-board serial integration test.

## Global Constraints

- **Never trust the wire:** every received record passes `learn_regate` (budget + Law-3 + shape_hash recompute) before merge, at both Vigil and the decoy.
- **Idempotent by shape_hash:** re-receiving a record must not corrupt state.
- **Reuse the AES-GCM transport** (`radar_wire_seal`/`open`, `SIMULACRA_ESPNOW_KEY`, `RADAR_FRAME_MAX 250`).
- **Both projects share `components/simulacra_radar/`** — component changes must compile for the decoy (C5, IDF 5.5) and Vigil (classic ESP32, IDF 5.4).
- **Gate the decoy side by `SIMULACRA_ESPNOW`** (the existing radar-link flag). Vigil always links the sync.

### First-cut design decisions (refinements over the high-level spec — flagged for review)

- **D1 — full-library idempotent sync, not delta.** Both directions send the *whole* library periodically (chunked). The spec's delta-up/dirty-tracking is deferred: it conflicts with `learned_template_t` being a fixed wire record (a `dirty` bit can't live in the record), and full+idempotent is correct and simple for small libraries. Airtime cost only.
- **D2 — wire merge takes `max(reinforce_count)`, not increment.** A re-broadcast of the same shape must not inflate its weight, so `learn_merge_wire` takes the max of the two counts (propagating a shape's established weight) while *local* observation still increments via `learn_merge`. Prevents runaway reinforcement under repeated sync.
- **D3 — single-peer replay.** Reuse one `radar_replay_t` per new frame type; multi-decoy replay is deferred (idempotent merge keeps replays harmless for correctness — the gate only avoids redundant work).
- **D4 — Vigil library is RAM-only** (`VIGIL_LIB_CAP` records, lost on reboot). SD persistence is Phase 2b-ii.

### Build / flash / read commands

Decoy (C5, COM12), selftest and normal builds (IDF 5.5):
```powershell
$env:PATH = "C:\Program Files\Python312;C:\Program Files\Python312\Scripts;$env:PATH"
$env:IDF_PATH = "~\esp\v5.5\esp-idf"; . "$env:IDF_PATH\export.ps1"
Set-Location ~\Downloads\splinter
idf.py -B build.c5 -DCHURN_SELFTEST=1 build                       # host-logic selftest
idf.py -B build.c5 -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 build  # decoy w/ radar link + sync
```
Vigil (CYD, COM10), classic ESP32 (IDF 5.4, separate shell):
```powershell
$env:IDF_PATH = "~\esp\v5.4\esp-idf"; . "$env:IDF_PATH\export.ps1"
Set-Location ~\Downloads\splinter\cyd
idf.py build ; idf.py -p COM10 flash
```
Serial read helper: `python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port <COM> --seconds <n> --reset yes`.

---

### Task 1: Shared `learn_merge_wire` (max-reinforce merge)

Add the wire-side merge that maxes reinforcement instead of incrementing, so re-broadcasts don't inflate weights.

**Files:**
- Modify: `components/simulacra_radar/learn_wire.h`
- Modify: `components/simulacra_radar/learn_wire.c`
- Modify: `main/churn_selftest.c` (`test_learn_merge_wire()`)

**Interfaces:**
- Consumes: `lw_find`/`lw_weakest` (Task 3 of Phase 2a, static in `learn_wire.c`).
- Produces: `bool learn_merge_wire(learned_template_t *store, size_t *count, size_t cap, const learned_template_t *rec, uint16_t sweep_no);` — like `learn_merge`, but on a dup it sets `reinforce_count = max(existing, rec->reinforce_count)` instead of incrementing.

- [ ] **Step 1: Write the failing test** in `churn_selftest.c` (call `test_learn_merge_wire();` after `test_learn_regate();`):

```c
static void test_learn_merge_wire(void)
{
    learned_template_t store[4]; size_t cnt = 0;
    learned_template_t a; mk_shape(&a, 0x0075); a.reinforce_count = 5;

    ST_CHECK(learn_merge_wire(store, &cnt, 4, &a, 1) && cnt == 1, "merge_wire: first insert");
    ST_CHECK(store[0].reinforce_count == 5, "merge_wire: keeps incoming weight on insert");

    // Re-merging the same record must NOT inflate the weight (max, not increment).
    learn_merge_wire(store, &cnt, 4, &a, 2);
    ST_CHECK(cnt == 1 && store[0].reinforce_count == 5, "merge_wire: dup keeps max (no inflation)");

    // A stronger copy raises it to the max.
    learned_template_t a2 = a; a2.reinforce_count = 9;
    learn_merge_wire(store, &cnt, 4, &a2, 3);
    ST_CHECK(store[0].reinforce_count == 9, "merge_wire: max raises weight");
}
```

- [ ] **Step 2: Build to verify it fails** — `idf.py -B build.c5 -DCHURN_SELFTEST=1 build`, expect `undefined reference to 'learn_merge_wire'`.

- [ ] **Step 3: Declare in `learn_wire.h`** (after `learn_merge`):

```c
// Like learn_merge, but a duplicate takes max(reinforce_count) instead of incrementing —
// used on the wire-receive path so re-broadcasts don't inflate a shape's weight.
bool learn_merge_wire(learned_template_t *store, size_t *count, size_t cap,
                      const learned_template_t *rec, uint16_t sweep_no);
```

- [ ] **Step 4: Implement in `learn_wire.c`** (after `learn_merge`):

```c
bool learn_merge_wire(learned_template_t *store, size_t *count, size_t cap,
                      const learned_template_t *rec, uint16_t sweep_no)
{
    int idx = lw_find(store, *count, rec->shape_hash);
    if (idx >= 0) {
        learned_template_t *e = &store[idx];
        if (rec->reinforce_count > e->reinforce_count) e->reinforce_count = rec->reinforce_count;
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

- [ ] **Step 5: Build + flash + read the decoy selftest** — expect `SELFTEST: PASS` with the `merge_wire:` checks.

- [ ] **Step 6: Build-check the CYD** — expect `Project build complete`.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/learn_wire.h components/simulacra_radar/learn_wire.c main/churn_selftest.c
git commit -m "feat(learn): learn_merge_wire (max-reinforce for wire-received records)"
```

---

### Task 2: Decoy store snapshot + wire-ingest API

Give the decoy a way to export its library (to offer) and ingest re-gated records (from sync).

**Files:**
- Modify: `main/learn.h`
- Modify: `main/learn.c`
- Modify: `main/churn_selftest.c` (`test_learn_snapshot_ingest()`)

**Interfaces:**
- Consumes: `learn_regate`, `learn_merge_wire` (Task 1), the decoy store (`s_store`/`s_count`).
- Produces:
  - `size_t learn_snapshot(learned_template_t *out, size_t max);` — copy up to `max` stored records into `out`, return the number copied.
  - `bool learn_ingest_wire(const learned_template_t *rec);` — `learn_regate(rec)` then `learn_merge_wire` into the decoy store at the current sweep; returns true iff accepted (regate passed and merge admitted it).

- [ ] **Step 1: Write the failing test** (`test_learn_snapshot_ingest();`):

```c
static void test_learn_snapshot_ingest(void)
{
    learn_reset();
    // Ingest two clean records as if received over the wire.
    learned_template_t a; mk_shape(&a, 0x0075); a.reinforce_count = 3;
    learned_template_t b; mk_shape(&b, 0x009E); b.reinforce_count = 1;
    ST_CHECK(learn_ingest_wire(&a) && learn_ingest_wire(&b), "ingest: two clean records accepted");
    ST_CHECK(learn_count() == 2, "ingest: store holds both");

    // A tampered record (forbidden subtype) is rejected by the re-gate.
    learned_template_t evil = a; uint8_t bad[9] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    memcpy(evil.ad, bad, 9); evil.ad_len = 9; evil.shape_hash = learn_shape_hash(&evil);
    ST_CHECK(!learn_ingest_wire(&evil), "ingest: forbidden record rejected");
    ST_CHECK(learn_count() == 2, "ingest: store unchanged after reject");

    // Snapshot exports what is held.
    learned_template_t out[8];
    size_t n = learn_snapshot(out, 8);
    ST_CHECK(n == 2, "snapshot: exports the stored count");
    learn_reset();
}
```

- [ ] **Step 2: Build to verify it fails** — `undefined reference to 'learn_snapshot'`.

- [ ] **Step 3: Declare in `main/learn.h`** (after the store section):

```c
// Fleet sync (Phase 2b): export/import library records.
size_t learn_snapshot(learned_template_t *out, size_t max);   // copy up to max records; returns count
bool   learn_ingest_wire(const learned_template_t *rec);      // regate + max-merge a received record
```

- [ ] **Step 4: Implement in `main/learn.c`** (near the store functions; `s_sweep` is the file-static sweep counter):

```c
size_t learn_snapshot(learned_template_t *out, size_t max)
{
    size_t n = (s_count < max) ? s_count : max;
    memcpy(out, s_store, n * sizeof(learned_template_t));
    return n;
}

bool learn_ingest_wire(const learned_template_t *rec)
{
    if (!learn_regate(rec)) return false;
    return learn_merge_wire(s_store, &s_count, LEARN_CAP, rec, s_sweep);
}
```

Add `#include "learn_wire.h"` if not already present (it is, from Phase 2a Task 3).

- [ ] **Step 5: Build + flash + read the decoy selftest** — expect `SELFTEST: PASS` with `ingest:`/`snapshot:` checks.

- [ ] **Step 6: Commit**

```bash
git add main/learn.h main/learn.c main/churn_selftest.c
git commit -m "feat(learn): decoy library snapshot + regated wire-ingest API"
```

---

### Task 3: Decoy — send `LEARN_OFFER`, receive `LEARN_SYNC`

Wire the sync into the decoy's `esp_now_link`: periodically offer the whole library, and merge any `LEARN_SYNC` received from Vigil.

**Files:**
- Modify: `main/esp_now_link.c`

**Interfaces:**
- Consumes: `learn_snapshot`, `learn_ingest_wire` (Task 2); `learn_wire_pack`/`unpack` (Phase 2a Task 4); `radar_wire_seal`/`open`.
- Produces: on-air `LEARN_OFFER` bursts and `LEARN_SYNC` ingestion (verified on-target in Task 5).

- [ ] **Step 1: Add a library-offer sender.** In `main/esp_now_link.c`, add `#include "learn.h"` and `#include "learn_wire.h"` near the top includes, and a helper (after `respond_once`):

```c
static radar_replay_t s_sync_replay;    // reject replayed LEARN_SYNC from Vigil

static void offer_library(void)
{
    static learned_template_t snap[LEARN_CAP];
    size_t n = learn_snapshot(snap, LEARN_CAP);
    if (n == 0) return;
    uint8_t chunks = (uint8_t)((n + LEARN_WIRE_RECS_PER_CHUNK - 1) / LEARN_WIRE_RECS_PER_CHUNK);
    for (uint8_t ci = 0; ci < chunks; ci++) {
        size_t off = (size_t)ci * LEARN_WIRE_RECS_PER_CHUNK;
        uint8_t nrec = (uint8_t)((n - off < LEARN_WIRE_RECS_PER_CHUNK) ? (n - off)
                                                                       : LEARN_WIRE_RECS_PER_CHUNK);
        uint8_t pl[RADAR_FRAME_MAX]; size_t plen;
        if (learn_wire_pack(pl, &plen, &snap[off], nrec, 1, ci, chunks) != 0) continue;
        uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
        if (radar_wire_seal(frame, &flen, RADAR_TYPE_LEARN_OFFER, pl, plen,
                            SIMULACRA_ESPNOW_KEY, s_salt, ++s_counter) == 0)
            esp_now_send(BCAST, frame, flen);
        vTaskDelay(pdMS_TO_TICKS(20));   // space chunks so the peer's RX queue drains
    }
    ESP_LOGW(ETAG, "offered library (%u recs, %u chunks)", (unsigned)n, chunks);
}
```

- [ ] **Step 2: Handle `LEARN_SYNC` in `on_recv`.** Extend the receive callback — after the existing `REQUEST` handling, add:

```c
    if (type == RADAR_TYPE_LEARN_SYNC) {
        if (!radar_replay_ok(&s_sync_replay, salt, ctr)) return;
        learn_chunk_hdr_t h; learned_template_t rx[LEARN_WIRE_RECS_PER_CHUNK]; uint8_t nr;
        if (learn_wire_unpack(pl, plen, rx, &nr, &h) != 0) return;
        for (uint8_t i = 0; i < nr; i++) learn_ingest_wire(&rx[i]);   // regate inside
        return;
    }
```

> `on_recv` currently `return`s after setting `s_answer` for a `REQUEST`. Restructure so a non-REQUEST type falls through to the `LEARN_SYNC` check instead of returning early: replace `if (type != RADAR_TYPE_REQUEST) return;` with `if (type == RADAR_TYPE_REQUEST) { ... s_answer = true; return; }` then the `LEARN_SYNC` block.

- [ ] **Step 3: Drive periodic offers from `espnow_task`.** Replace the task body so it offers the library on a timer alongside answering requests:

```c
static void espnow_task(void *arg)
{
    (void)arg;
    uint32_t last_offer = 0;
    for (;;) {
        if (s_answer) { s_answer = false; respond_once(); }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_offer > 30000) { last_offer = now; offer_library(); }   // every 30 s
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

- [ ] **Step 4: Build the decoy with the radar link** — `idf.py -B build.c5 -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 build`. Expect `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add main/esp_now_link.c
git commit -m "feat(espnow): decoy offers its library (LEARN_OFFER) + ingests LEARN_SYNC"
```

---

### Task 4: Vigil — receive `LEARN_OFFER`, merge, broadcast `LEARN_SYNC`

Give Vigil a RAM library that ingests offers (re-gated) and rebroadcasts the top-N.

**Files:**
- Modify: `cyd/main/cyd_main.c`

**Interfaces:**
- Consumes: `learn_wire_unpack`/`pack`, `learn_regate`, `learn_merge_wire` (shared component); `radar_wire_seal`/`open`.
- Produces: on-air `LEARN_SYNC` bursts + a RAM library (verified on-target in Task 5).

- [ ] **Step 1: Add the RAM library + includes.** In `cyd/main/cyd_main.c`, add `#include "learn_wire.h"` near the includes, and after the `s_status` statics:

```c
#define VIGIL_LIB_CAP 128
static learned_template_t s_lib[VIGIL_LIB_CAP];
static size_t             s_lib_count;
static radar_replay_t     s_offer_replay;   // reject replayed LEARN_OFFER
static uint16_t           s_lib_sweep;      // local "time" for merges/age (monotonic tick)
```

- [ ] **Step 2: Handle `LEARN_OFFER` in `on_recv`.** After the `STATUS` handling block (which `return`s), the function currently ends. Change the early `STATUS` guard so other types fall through: replace `if (type!=RADAR_TYPE_STATUS || plen!=sizeof(radar_wire_status_t)) return;` with a dispatch:

```c
    if (type == RADAR_TYPE_STATUS && plen == sizeof(radar_wire_status_t)) {
        if (!radar_replay_ok(&s_replay,salt,ctr)) return;
        memcpy(&s_status, pl, sizeof s_status);
        if (s_status.threat_count > RADAR_MAX_THREATS) s_status.threat_count = RADAR_MAX_THREATS;
        s_status_ms = (uint32_t)(esp_timer_get_time()/1000);
        return;
    }
    if (type == RADAR_TYPE_LEARN_OFFER) {
        if (!radar_replay_ok(&s_offer_replay, salt, ctr)) return;
        learn_chunk_hdr_t h; learned_template_t rx[LEARN_WIRE_RECS_PER_CHUNK]; uint8_t nr;
        if (learn_wire_unpack(pl, plen, rx, &nr, &h) != 0) return;
        for (uint8_t i = 0; i < nr; i++)
            if (learn_regate(&rx[i]))
                learn_merge_wire(s_lib, &s_lib_count, VIGIL_LIB_CAP, &rx[i], s_lib_sweep);
        ESP_LOGW(TAG, "offer rx: +%u recs, lib=%u", (unsigned)nr, (unsigned)s_lib_count);
        return;
    }
```

(Keep the existing `ESP_LOGW status rx` inside the STATUS block.)

- [ ] **Step 3: Add a top-N `LEARN_SYNC` broadcaster.** After `send_request`, add:

```c
static void broadcast_library(void)
{
    if (s_lib_count == 0) return;
    s_lib_sweep++;
    // top-N by reinforce_count: simple selection into a send buffer (N == whole lib here, cap fits chunks)
    uint8_t chunks = (uint8_t)((s_lib_count + LEARN_WIRE_RECS_PER_CHUNK - 1) / LEARN_WIRE_RECS_PER_CHUNK);
    for (uint8_t ci = 0; ci < chunks; ci++) {
        size_t off = (size_t)ci * LEARN_WIRE_RECS_PER_CHUNK;
        uint8_t nrec = (uint8_t)((s_lib_count - off < LEARN_WIRE_RECS_PER_CHUNK)
                                 ? (s_lib_count - off) : LEARN_WIRE_RECS_PER_CHUNK);
        uint8_t pl[RADAR_FRAME_MAX]; size_t plen;
        if (learn_wire_pack(pl, &plen, &s_lib[off], nrec, 1, ci, chunks) != 0) continue;
        uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
        if (radar_wire_seal(frame, &flen, RADAR_TYPE_LEARN_SYNC, pl, plen,
                            SIMULACRA_ESPNOW_KEY, s_salt, ++s_ctr) == 0)
            esp_now_send(BCAST, frame, flen);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "broadcast library (%u recs)", (unsigned)s_lib_count);
}
```

> This sends the whole RAM library; since `VIGIL_LIB_CAP` (128) already fits the decoy caps, "top-N" degenerates to "all" here. A real top-N selection (rank by `reinforce_count`, cap to the smallest decoy) is a Phase 2b-ii refinement when SD lets the library exceed a decoy cap.

- [ ] **Step 4: Drive periodic broadcast from `app_main`.** In the main loop (near the `send_request` cadence at `now-last_req > 1000`), add a slower library-broadcast timer:

```c
        static uint32_t last_sync = 0;
        if (now - last_sync > 20000) { last_sync = now; broadcast_library(); }   // every 20 s
```

- [ ] **Step 5: Build the CYD** — `idf.py build` in `cyd/`. Expect `Project build complete`.

- [ ] **Step 6: Commit**

```bash
git add cyd/main/cyd_main.c
git commit -m "feat(vigil): RAM library — ingest LEARN_OFFER, broadcast LEARN_SYNC"
```

---

### Task 5: On-target two-board integration test

Prove a shape learned on the decoy propagates to Vigil and back.

**Files:** none (verification only).

- [ ] **Step 1: Flash Vigil (CYD, COM10).** Build + flash the CYD; start a serial read in one shell:

```powershell
# (IDF 5.4 shell) in cyd/:
idf.py -p COM10 flash
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM10 --seconds 90 --reset yes --grep "offer rx|broadcast library|status rx"
```

- [ ] **Step 2: Flash the decoy in observe-fed learn mode (C5, COM12)** so it learns quickly and the radar link is on. Use observe mode (continuous scan → fast learning) with the ESP-NOW link:

```powershell
# (IDF 5.5 shell):
idf.py -B build.c5 -DCHURN_SELFTEST=0 -DSIMULACRA_OBSERVE=1 -DSIMULACRA_ESPNOW=1 build
idf.py -B build.c5 -DCHURN_SELFTEST=0 -DSIMULACRA_OBSERVE=1 -DSIMULACRA_ESPNOW=1 -p COM12 flash
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM12 --seconds 90 --reset yes --grep "learn:|offered library|espnow"
```

> Note: observe mode drives `learn_offer` continuously, so the decoy accrues a library within a sweep or two; `esp_now_link` then offers it every 30 s. If observe mode does not start the ESP-NOW responder in the current main dispatch, temporarily start it from the observe branch, or run the normal decoy (`-DSIMULACRA_OBSERVE=0`) and wait for a reprofile window. Confirm on the bench which path drives both learning and the link.

- [ ] **Step 3: Verify the round-trip in the serial logs.** Expected within ~1–2 minutes:
  - Decoy: `+shape ...` (learned), then `offered library (N recs, M chunks)`.
  - Vigil: `offer rx: +K recs, lib=N`, then `broadcast library (N recs)`.
  - Decoy: after Vigil's broadcast, subsequent `offered library` counts hold steady (its own shapes echoed back merge idempotently — no growth from the echo).
  - To prove cross-pollination with two decoys is out of scope here (single decoy); the single-decoy proof is: decoy → Vigil (`lib` grows to the decoy's count) → Vigil broadcast accepted by the decoy without inflating its count.

- [ ] **Step 4: Restore boards.** Reflash the decoy to the normal Ward build (`-DSIMULACRA_OBSERVE=0 -DSIMULACRA_ESPNOW=1` if keeping the display pairing, else defaults) and leave Vigil on the new firmware.

- [ ] **Step 5 (optional): Commit any tweak** made during Step 2's bench confirmation (e.g., starting the link from the observe branch). Otherwise nothing to commit.

---

## Self-Review

**Spec coverage (sync-core subset of `2026-07-03-fleet-template-sync-design.md`):**
- `LEARN_OFFER` decoy→Vigil + `LEARN_SYNC` Vigil→all (spec §Sync model) → Tasks 3–4.
- Re-gate every received record at both ends (spec §principle 2) → Tasks 2–4 (`learn_regate` in ingest paths).
- Idempotent merge by shape_hash (spec §Merge) → Task 1 (`learn_merge_wire`, max-reinforce refinement D2).
- Graceful degradation — decoy works standalone, sync is additive (spec §principle 1) → the sync path never blocks the decoy; offers only fire when a library exists.
- **Deferred to Phase 2b-ii:** SD-backed encrypted persistence (spec §SD storage), dirty-delta uplink (spec §Sync model — replaced here by full-library per D1), true top-N ranking when the Vigil library exceeds a decoy cap, Vigil UI page, multi-decoy replay.

**Placeholder scan:** every code step is complete. Task 5 Step 2 flags a genuine on-bench branch decision (which build drives both learning and the link) rather than leaving code vague.

**Type consistency:** `learn_merge_wire(store,&count,cap,rec,sweep)` consistent across Task 1 def and Tasks 2/4 uses. `learn_snapshot(out,max)`/`learn_ingest_wire(rec)` consistent between Task 2 def and Task 3 use. `LEARN_WIRE_RECS_PER_CHUNK`, `learn_chunk_hdr_t`, `RADAR_TYPE_LEARN_OFFER/SYNC` from Phase 2a used consistently. `s_salt`/`s_counter` (decoy) and `s_salt`/`s_ctr` (Vigil) match each file's existing sealing state.
