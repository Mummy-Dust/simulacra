# Fleet Template Sync — Phase 2b-iii part 1 (Vigil LIBRARY page + changed-aware merge) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the librarian role visible (spec §Vigil UI: one new touch page with library size, last merge/sync time, SD mount status) and stop the identical-blob re-save every 30s in an active fleet.

**Architecture:** `learn_merge_wire` gains "changed" return semantics (true only when the store materially changed) so Vigil's existing `dirty` gate becomes correct for free. A fourth `RADAR_VIEW_LIBRARY` view joins the shared page-cycle; the shared renderer gains a `radar_lib_info_t` snapshot struct (nullable — decoy displays pass NULL) and a `draw_library` page. Vigil tracks last-offer/last-sync/last-save timestamps and feeds the struct each frame.

**Tech Stack:** ESP-IDF v5.5 (C5 decoy selftest, COM12) / v5.4 (CYD Vigil, COM10), shared `components/simulacra_radar`, on-target selftest harness `main/churn_selftest.c` (`ST_CHECK`).

## Global Constraints

- Never trust the wire: re-gate stays in place on every hop (unchanged here).
- `learned_template_t` stays a packed pointer-free POD (unchanged here).
- Build gates via `idf.py -D` are forwarded by the `main/CMakeLists.txt` foreach block; selftest build = `-DCHURN_SELFTEST=1`.
- Vigil is receive/merge/broadcast only; it never advertises.
- Commits carry AI trailers → scrub before any push (repo policy).

---

### Task 1: `learn_merge_wire` returns *changed*, not *present*

**Files:**
- Modify: `components/simulacra_radar/learn_wire.h:11-14` (doc comment)
- Modify: `components/simulacra_radar/learn_wire.c:58-75` (`learn_merge_wire`)
- Modify: `main/learn.h:51` (doc comment), `main/learn.c:151-155` (comment only)
- Test: `main/churn_selftest.c` (`test_learn_merge_wire`)

**Interfaces:**
- Consumes: existing `learn_merge_wire(store,count,cap,rec,sweep)` and `lw_find`.
- Produces: same signature, new contract — returns **true iff the store materially changed** (insert, eviction-replace, reinforce raised, or interval band widened). A pure duplicate no-op (only `last_seen_sweep` refreshed) and a rejected-weakest both return false. `learn_ingest_wire` inherits the contract (regate-reject → false; unchanged-dup → false).

- [x] **Step 1: Extend the failing test**

In `main/churn_selftest.c`, replace the body of `test_learn_merge_wire` with:

```c
static void test_learn_merge_wire(void)
{
    learned_template_t store[4]; size_t cnt = 0;
    learned_template_t a; mk_shape(&a, 0x0075); a.reinforce_count = 5;

    ST_CHECK(learn_merge_wire(store, &cnt, 4, &a, 1) && cnt == 1, "merge_wire: first insert");
    ST_CHECK(store[0].reinforce_count == 5, "merge_wire: keeps incoming weight on insert");

    // Re-merging the same record must NOT inflate the weight (max, not increment) —
    // and a pure no-op duplicate reports "unchanged" so Vigil doesn't re-save the blob.
    ST_CHECK(!learn_merge_wire(store, &cnt, 4, &a, 2), "merge_wire: no-op dup reports unchanged");
    ST_CHECK(cnt == 1 && store[0].reinforce_count == 5, "merge_wire: dup keeps max (no inflation)");

    // A stronger copy raises it to the max — that IS a change.
    learned_template_t a2 = a; a2.reinforce_count = 9;
    ST_CHECK(learn_merge_wire(store, &cnt, 4, &a2, 3), "merge_wire: max raise reports changed");
    ST_CHECK(store[0].reinforce_count == 9, "merge_wire: max raises weight");

    // Widening the interval band is also a durable change.
    learned_template_t a3 = store[0]; a3.itvl_max_ms = store[0].itvl_max_ms + 100;
    ST_CHECK(learn_merge_wire(store, &cnt, 4, &a3, 4), "merge_wire: interval widen reports changed");
}
```

- [x] **Step 2: Run selftest build to verify it fails**

Run (C5, IDF 5.5 env): `idf.py -p COM12 -DCHURN_SELFTEST=1 build flash monitor`
Expected: FAIL on "merge_wire: no-op dup reports unchanged" (current code returns true).

- [x] **Step 3: Implement changed-aware merge**

In `components/simulacra_radar/learn_wire.c`, replace the duplicate branch of `learn_merge_wire`:

```c
    int idx = lw_find(store, *count, rec->shape_hash);
    if (idx >= 0) {
        learned_template_t *e = &store[idx];
        bool changed = false;
        if (rec->reinforce_count > e->reinforce_count) { e->reinforce_count = rec->reinforce_count; changed = true; }
        e->last_seen_sweep = sweep_no;                   // freshness only — not a durable change
        if (rec->itvl_min_ms < e->itvl_min_ms) { e->itvl_min_ms = rec->itvl_min_ms; changed = true; }
        if (rec->itvl_max_ms > e->itvl_max_ms) { e->itvl_max_ms = rec->itvl_max_ms; changed = true; }
        return changed;
    }
```

Update the header comment in `learn_wire.h`:

```c
// Like learn_merge, but a duplicate takes max(reinforce_count) instead of incrementing —
// used on the wire-receive path so re-broadcasts don't inflate a shape's weight.
// Returns true iff the store materially changed (insert, replace, weight raise, or
// interval widen); a pure no-op duplicate returns false so callers can gate persistence.
```

And `main/learn.h:51` comment: `// regate + max-merge; true iff the store changed`.

- [x] **Step 4: Run selftest to verify it passes**

Run: `idf.py -p COM12 -DCHURN_SELFTEST=1 build flash monitor`
Expected: all checks PASS (count grows by +2 over 259).

- [x] **Step 5: Commit**

```bash
git add components/simulacra_radar/learn_wire.{h,c} main/learn.h main/churn_selftest.c
git commit -m "fix(learn): merge_wire reports material change, not presence (stops Vigil no-op re-saves)"
```

---

### Task 2: shared `RADAR_VIEW_LIBRARY` page

**Files:**
- Modify: `components/simulacra_radar/radar_ui.h:8` (enum)
- Modify: `components/simulacra_radar/radar_render.h` (struct + signature)
- Modify: `components/simulacra_radar/radar_render.c` (`draw_library`)
- Test: `main/churn_selftest.c` (`test_radar_ui`)

**Interfaces:**
- Consumes: existing `radar_gfx_text`, `radar_gfx_clear`, view-cycle in `radar_ui_on_input`.
- Produces:
  ```c
  typedef struct {                 // Vigil librarian snapshot for the LIBRARY page
      bool     sd_ok;
      uint32_t card_mb;            // capacity, 0 if unknown/absent
      uint16_t lib_count, lib_cap;
      uint32_t offer_age_s;        // UINT32_MAX = never
      uint32_t sync_age_s;         // UINT32_MAX = never
      uint32_t save_age_s;         // UINT32_MAX = never
      uint32_t save_bytes;         // size of last sealed blob
  } radar_lib_info_t;
  void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                         const radar_lib_info_t *lib,   // NULL on non-librarian displays
                         uint16_t sweep_deg, uint16_t *band, int band_h, int w, int h,
                         radar_flush_fn flush, void *ctx);
  ```
  New enum member `RADAR_VIEW_LIBRARY` between `RADAR_VIEW_STATS` and `RADAR_VIEW_COUNT`.

- [x] **Step 1: Update the failing test**

In `test_radar_ui` (`main/churn_selftest.c:969`), the cycle assertions become:

```c
    radar_ui_on_input(&ui, 1100); ST_CHECK(ui.view == RADAR_VIEW_DETAIL,  "input 1 -> detail");
    radar_ui_on_input(&ui, 1200); ST_CHECK(ui.view == RADAR_VIEW_STATS,   "input 2 -> stats");
    radar_ui_on_input(&ui, 1250); ST_CHECK(ui.view == RADAR_VIEW_LIBRARY, "input 3 -> library");
    radar_ui_on_input(&ui, 1300); ST_CHECK(ui.view == RADAR_VIEW_RADAR,   "input 4 -> wraps");
    radar_ui_on_input(&ui, 2000); radar_ui_on_input(&ui, 2000);           // -> stats
```

(The idle-return checks below that line are unchanged: stats still idle-returns to radar.)

- [x] **Step 2: Run selftest build to verify it fails**

Run: `idf.py -p COM12 -DCHURN_SELFTEST=1 build`
Expected: FAIL to compile — `RADAR_VIEW_LIBRARY` undeclared.

- [x] **Step 3: Implement enum, struct, and page**

`radar_ui.h:8`:

```c
typedef enum { RADAR_VIEW_RADAR = 0, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS, RADAR_VIEW_LIBRARY, RADAR_VIEW_COUNT } radar_view_t;
```

`radar_render.h`: add the `radar_lib_info_t` struct (as in Interfaces above, with `#include <stdbool.h>` via radar_ui.h already) and the new `radar_render_view` signature.

`radar_render.c`: add after `draw_stats`:

```c
static void fmt_age(char *out, size_t n, const char *label, uint32_t age_s){
    if (age_s == UINT32_MAX) snprintf(out, n, "%s never", label);
    else                     snprintf(out, n, "%s %lus ago", label, (unsigned long)age_s);
}
static void draw_library(radar_gfx_t *g, const radar_lib_info_t *lib){
    char l[40]; int y=6; radar_gfx_text(g,8,y,"LIBRARY",COL_FG); y+=24;
    #define ROW(...) do{ snprintf(l,sizeof l,__VA_ARGS__); radar_gfx_text(g,6,y,l,COL_DIM); y+=18; }while(0)
    if (!lib) { radar_gfx_text(g,6,y,"not a librarian",COL_DIM); return; }
    if (lib->sd_ok) ROW("sd OK %luMB",(unsigned long)lib->card_mb);
    else            radar_gfx_text(g,6,y,"sd ABSENT (RAM only)",COL_WARN), y+=18;
    ROW("lib %u/%u shapes",(unsigned)lib->lib_count,(unsigned)lib->lib_cap);
    fmt_age(l,sizeof l,"offer rx",lib->offer_age_s); radar_gfx_text(g,6,y,l,COL_DIM); y+=18;
    fmt_age(l,sizeof l,"sync tx ",lib->sync_age_s);  radar_gfx_text(g,6,y,l,COL_DIM); y+=18;
    if (lib->save_age_s == UINT32_MAX) ROW("save never");
    else ROW("save %lus ago (%luB)",(unsigned long)lib->save_age_s,(unsigned long)lib->save_bytes);
    #undef ROW
}
```

(`#include <stdint.h>` for UINT32_MAX comes via radar_ui.h.) Dispatch in `radar_render_view`:

```c
void radar_render_view(radar_view_t view, const radar_wire_status_t *st, const radar_lib_info_t *lib,
                       uint16_t sweep, uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx){
    for(int y0=0;y0<h;y0+=band_h){ radar_gfx_t g={ .buf=band, .w=w, .y0=y0, .h=band_h };
        radar_gfx_clear(&g,COL_BG);
        if(view==RADAR_VIEW_DETAIL) draw_detail(&g,st);
        else if(view==RADAR_VIEW_STATS) draw_stats(&g,st);
        else if(view==RADAR_VIEW_LIBRARY) draw_library(&g,lib);
        else draw_radar(&g,st,sweep);
        flush(y0, band_h, band, ctx); }
}
```

- [x] **Step 4: Run selftest to verify it passes**

Run: `idf.py -p COM12 -DCHURN_SELFTEST=1 build flash monitor`
Expected: all PASS (+1 check over Task 1's count). The CYD is NOT yet updated — that is Task 3; the decoy selftest build doesn't call `radar_render_view` so it links clean.

- [x] **Step 5: Commit**

```bash
git add components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_render.{h,c} main/churn_selftest.c
git commit -m "feat(radar): LIBRARY page in the shared view cycle (librarian visibility)"
```

---

### Task 3: Vigil wiring — timestamps + lib info

**Files:**
- Modify: `cyd/main/cyd_main.c`

**Interfaces:**
- Consumes: `radar_lib_info_t`, new `radar_render_view` signature, `s_card->csd` from Task 2b-ii mount.
- Produces: file-scope `s_last_offer_ms`, `s_last_sync_ms`, `s_last_save_ms`, `s_save_bytes` (0 = never for the `_ms` trio; ages computed as `(now-ts)/1000`, `UINT32_MAX` when never).

- [x] **Step 1: Track event times**

In `cyd_main.c` near `s_lib_dirty` add:

```c
static uint32_t s_last_offer_ms, s_last_sync_ms, s_last_save_ms;  // 0 = never
static uint32_t s_save_bytes;
```

In `on_recv` LEARN_OFFER branch, after the merge loop:
`s_last_offer_ms = (uint32_t)(esp_timer_get_time()/1000);`

In `broadcast_library`, before the final log line:
`s_last_sync_ms = (uint32_t)(esp_timer_get_time()/1000);`

In `learn_db_save`, after the "saved" log line:
`s_last_save_ms = (uint32_t)(esp_timer_get_time()/1000); s_save_bytes = (uint32_t)blen;`

- [x] **Step 2: Feed the page each frame**

Add a helper above `app_main`:

```c
static uint32_t age_s(uint32_t now, uint32_t ts){ return ts ? (uint32_t)(now - ts)/1000u : UINT32_MAX; }
```

In the render branch of the main loop, replace the `radar_render_view` call with:

```c
            radar_lib_info_t lib = {
                .sd_ok = s_sd_ok,
                .card_mb = s_sd_ok ? (uint32_t)(((uint64_t)s_card->csd.capacity)*s_card->csd.sector_size/(1024*1024)) : 0,
                .lib_count = (uint16_t)s_lib_count, .lib_cap = VIGIL_LIB_CAP,
                .offer_age_s = age_s(now, s_last_offer_ms),
                .sync_age_s  = age_s(now, s_last_sync_ms),
                .save_age_s  = age_s(now, s_last_save_ms),
                .save_bytes  = s_save_bytes,
            };
            radar_render_view(ui.view, &s_status, &lib, sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
```

- [x] **Step 3: Build the CYD firmware**

Run (IDF 5.4 env, from `cyd/`): `idf.py build`
Expected: clean build, no DRAM overflow (struct is stack, ~28 B).

- [x] **Step 4: Commit**

```bash
git add cyd/main/cyd_main.c
git commit -m "feat(vigil): LIBRARY touch page — sd/lib/offer/sync/save at a glance"
```

---

### Task 4: hardware verification

**Files:** none (bench).

- [x] **Step 1: Flash both boards**

- CYD (IDF 5.4): `idf.py -p COM10 flash monitor`
- Decoy C5 (IDF 5.5): normal selftest build already flashed from Task 2 leaves 2 shapes in NVS; reflash the display-paired Ward build `idf.py -p COM12 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1 build flash` so it offers its library every 30s.

- [x] **Step 2: Verify the page**

Touch the CYD screen 3× from radar → expect LIBRARY page showing `sd OK <N>MB`, `lib 2/128`, offer/sync/save ages counting. Page idle-returns to radar after 15 s.

- [x] **Step 3: Verify the re-save fix**

Watch the CYD monitor ≥3 offer cycles (~2 min): expect exactly ONE `learndb: saved ...` after the first offer that changes the lib (or zero if the on-card lib already matches), and NO repeated identical saves every 30 s thereafter while `offer rx` lines keep arriving.

- [x] **Step 4: Update plan checkboxes + finish**

Mark tasks done; then use superpowers:finishing-a-development-branch (verify selftest, offer merge options).

**HW verification (2026-07-03, C5 COM12 decoy + CYD COM10 Vigil):**
- Re-save fix CONFIRMED: CYD receives `offer rx` bursts every ~30s (decoy's full 13-rec library, 5 chunks), `lib` stays constant at 13, and **zero** `learndb: saved` lines across 100s — no-op duplicate offers no longer re-mark the lib dirty. (Pre-fix behavior was an identical re-save every 30s.)
- Bidirectional fleet sync observed: decoy library grew 2→13 by merging the CYD's `broadcast top-13` syncs, then offered all 13 back — all deduped by shape_hash on the CYD.
- Cache gotcha hit: a stale `CHURN_SELFTEST:UNINITIALIZED=1` from Task 1's `-DCHURN_SELFTEST=1` build persisted; the display-paired flash must pass `-DCHURN_SELFTEST=0` explicitly to override it.
- NOT machine-verifiable here: the LIBRARY page's on-screen appearance (needs a physical 3× touch to cycle to it). Render path builds clean and is dispatched per-frame; needs a human eyeball.
