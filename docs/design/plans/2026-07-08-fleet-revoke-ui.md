# Fleet Revoke UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Vigil-local modal roster on the CYD to select an enrolled decoy and revoke it (remove from allowlist + rotate fleet key + reopen enroll window).

**Architecture:** All new UI is Vigil-local in `cyd/main/cyd_main.c`, drawn with existing `radar_gfx` primitives in full-width 40px bands (no full framebuffer; classic-ESP32 DRAM can't spare 150 KB). The shared renderer is untouched. The revoke action reuses the existing `fleet_db` backend. Entry is a full-width "FLEET ROSTER" bar on the CONTROL page; while the modal is open it owns all touch input.

**Tech Stack:** ESP-IDF v5.4 (CYD / esp32), `radar_gfx`, `fleet_db`, TweetNaCl (already vendored). No new dependencies.

## Global Constraints

- Vigil (CYD) build flags: `-DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1` (+ `-DFLEET_SELFTEST=1` for the selftest build). All new code gated on `SIMULACRA_FLEET_PROVISION`; gate OFF must still compile.
- `-D` gate flags reach the compiler ONLY via the `foreach` forwarder in `cyd/main/CMakeLists.txt`. `#ifdef`-gated flags (like `FLEET_SELFTEST`) are cleared only by `idf.py fullclean` (passing `=0` still leaves them defined).
- AI trailers STAY in commits: end each commit body with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. PR body ends with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- Push via `git -c credential.helper= -c credential.helper='!gh auth git-credential' push`.
- No wire/protocol changes, no shared-renderer (`components/simulacra_radar/`) changes.
- Panel geometry: `LCD_W=240`, `LCD_H=320`. Font is 8×8 with an 8px advance (`radar_gfx_text`). `cyd_flush(y0, h, band, NULL)` draws full-width rows `[y0, y0+h)`. Touch maps to screen coords `(px, py)` via the existing `TCAL_*` transform in `touch_read_xy`-fed code.
- Fingerprint helper already present: `enroll_fp(char *out, size_t cap, const uint8_t idpk[32])` → `"xxxx-xxxx-xxxx-xxxx"` (19 chars).
- Backend already present: `fleet_allow_count()`, `fleet_allow_at(size_t)`, `fleet_allow_remove(const uint8_t[32])`, `fleet_db_rotate()`, `fleet_db_save()`, `fleet_db_epoch()`, `enroll_open_window(uint32_t now)`.

---

## File Structure

- `cyd/main/fleet_db.c` — MODIFY: extend `fleet_db_selftest` with a composite revoke assertion.
- `cyd/main/cyd_main.c` — MODIFY: modal state + `draw_btn`/`draw_fleet_modal`/`draw_fleet_bar` renderers + `fleet_modal_touch`/`fleet_do_revoke` + app_main loop integration.

No new files.

---

### Task 1: Composite revoke assertion in fleet_db_selftest

**Files:**
- Modify: `cyd/main/fleet_db.c` (inside `fleet_db_selftest`, before the final `ESP_LOGW`/`return`)

**Interfaces:**
- Consumes: `fleet_allow_add/contains/remove/count`, `fleet_db_rotate`, `s_key`, `s_epoch` (file statics).
- Produces: nothing new; strengthens the existing `fleet_db selftest` count.

- [ ] **Step 1: Add the composite check.** In `cyd/main/fleet_db.c`, locate the end of `fleet_db_selftest` (the block after the rotate check `uint32_t e0 = s_epoch; fleet_db_rotate(); ...`). Immediately before `ESP_LOGW(TAG, "fleet_db selftest: ...")`, insert:

```c
    // Composite "revoke" = remove target + rotate key (what the UI's fleet_do_revoke performs).
    s_allow_n = 0; s_epoch = 4; esp_fill_random(s_key, 32);      // fresh: 2 enrolled
    uint8_t rvk_key0[32]; 
    fleet_allow_add(a); fleet_allow_add(b);
    memcpy(rvk_key0, s_key, 32);
    uint32_t rvk_ep0 = s_epoch;
    // revoke 'a'
    fleet_allow_remove(a); fleet_db_rotate();
    if (fleet_allow_contains(a)) fail++;                         // target gone
    if (!fleet_allow_contains(b)) fail++;                        // survivor kept
    if (fleet_allow_count() != 1) fail++;                        // exactly one left
    if (s_epoch != rvk_ep0 + 1) fail++;                          // rotated
    if (memcmp(s_key, rvk_key0, 32) == 0) fail++;                // new key
```

- [ ] **Step 2: Build the CYD selftest firmware to compile-verify.**

Run (PowerShell, IDF v5.4 env):
```
idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 -DFLEET_SELFTEST=1 build
```
Expected: build completes; `fleet_db.c` recompiles clean.

- [ ] **Step 3: Commit.**

```
git add cyd/main/fleet_db.c
git commit -m "test(fleet): composite revoke (remove+rotate) assertion in fleet_db_selftest

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Modal state + renderers

**Files:**
- Modify: `cyd/main/cyd_main.c` (add statics + render helpers near the other `SIMULACRA_FLEET_PROVISION` enrollment code, e.g. after `enroll_rotate`)

**Interfaces:**
- Consumes: `fleet_allow_count/at`, `enroll_fp`, `fleet_db_epoch`, `radar_gfx_*`, `cyd_flush`, `LCD_W`, `LCD_H`.
- Produces: `static bool s_fleet_modal; static int s_fleet_sel, s_fleet_scroll; static uint32_t s_fleet_arm_ms;` and `static void draw_fleet_modal(uint16_t *band, uint32_t now);`, `static void draw_fleet_bar(uint16_t *band);` (consumed by Task 4).

- [ ] **Step 1: Add modal state + layout constants + a button helper.** Insert (inside the `#ifdef SIMULACRA_FLEET_PROVISION` region, after `enroll_rotate`):

```c
// ---- Fleet revoke modal (Vigil-local, drawn in full-width 40px bands) ----
#define FLEET_ROWS_VISIBLE 9        // rows between the header and the button band
#define FLEET_ROW_Y0       40       // first row top
#define FLEET_ROW_H        22
#define FLEET_BTN_Y        258      // UP/DOWN/REVOKE band top
#define FLEET_EXIT_Y       290      // EXIT band top
static bool     s_fleet_modal;      // roster modal open
static int      s_fleet_sel;        // selected allowlist index
static int      s_fleet_scroll;     // top visible index
static uint32_t s_fleet_arm_ms;     // REVOKE armed at (0 = disarmed); 3 s confirm window

static void draw_btn(radar_gfx_t *g, int x, int y, int w, const char *label, uint16_t bg){
    radar_gfx_fill_rect(g, x+2, y, w-4, 26, bg);
    int tx = x + (w - (int)strlen(label) * 8) / 2;
    radar_gfx_text(g, tx, y + 9, label, 0xFFFF);
}
```

- [ ] **Step 2: Add the full-screen modal renderer.**

```c
static void draw_fleet_modal(uint16_t *band, uint32_t now){
    int n = (int)fleet_allow_count();
    if (s_fleet_sel >= n) s_fleet_sel = n > 0 ? n - 1 : 0;
    if (s_fleet_sel < 0) s_fleet_sel = 0;
    if (s_fleet_sel < s_fleet_scroll) s_fleet_scroll = s_fleet_sel;
    if (s_fleet_sel >= s_fleet_scroll + FLEET_ROWS_VISIBLE)
        s_fleet_scroll = s_fleet_sel - FLEET_ROWS_VISIBLE + 1;
    bool armed = s_fleet_arm_ms && (uint32_t)(now - s_fleet_arm_ms) < 3000;

    for (int y0 = 0; y0 < LCD_H; y0 += 40){
        radar_gfx_t g = { band, LCD_W, y0, 40 };
        radar_gfx_clear(&g, 0x0000);
        char l[40];
        snprintf(l, sizeof l, "FLEET ROSTER (%d)", n);
        radar_gfx_text(&g, 8, 8, l, 0xFFFF);
        radar_gfx_hline(&g, 0, LCD_W - 1, 30, 0x7BEF);
        if (n == 0){
            radar_gfx_text(&g, 8, 60, "no decoys enrolled", 0xC618);
        } else {
            for (int r = 0; r < FLEET_ROWS_VISIBLE; r++){
                int idx = s_fleet_scroll + r;
                if (idx >= n) break;
                int ry = FLEET_ROW_Y0 + r * FLEET_ROW_H;
                if (idx == s_fleet_sel) radar_gfx_fill_rect(&g, 4, ry - 2, LCD_W - 8, 20, 0x001F);
                char fp[24]; enroll_fp(fp, sizeof fp, fleet_allow_at(idx));
                radar_gfx_text(&g, 14, ry + 2, fp, idx == s_fleet_sel ? 0xFFFF : 0xC618);
            }
        }
        if (n > 0){
            draw_btn(&g, 0,   FLEET_BTN_Y, 80, "UP",   0x39C7);
            draw_btn(&g, 80,  FLEET_BTN_Y, 80, "DOWN", 0x39C7);
            draw_btn(&g, 160, FLEET_BTN_Y, 80, armed ? "CONFIRM?" : "REVOKE", armed ? 0xF800 : 0x39C7);
        }
        draw_btn(&g, 0, FLEET_EXIT_Y, 240, "EXIT", 0x39C7);
        cyd_flush(y0, 40, band, NULL);
    }
}
```

- [ ] **Step 3: Add the CONTROL-page entry bar renderer.**

```c
// Full-width entry bar drawn at the top of the CONTROL page (tap to open the roster).
static void draw_fleet_bar(uint16_t *band){
    radar_gfx_t g = { band, LCD_W, 0, 40 };
    radar_gfx_clear(&g, 0x0000);
    radar_gfx_fill_rect(&g, 2, 2, LCD_W - 4, 24, 0x02D4);   // dark teal tab
    radar_gfx_text(&g, 40, 10, "[ FLEET ROSTER ]", 0xFFFF); // 16 ch, centered-ish
    cyd_flush(0, 28, band, NULL);
}
```

- [ ] **Step 4: Build to compile-verify.**

Run: `idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build`
Expected: `cyd_main.c` compiles (functions are static; a `-Wunused-function` warning is acceptable until Task 4 wires them in — they will be used there).

- [ ] **Step 5: Commit.**

```
git add cyd/main/cyd_main.c
git commit -m "feat(fleet): revoke-modal state + roster/bar renderers (Vigil-local)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Modal input + revoke action

**Files:**
- Modify: `cyd/main/cyd_main.c` (after `draw_fleet_bar`)

**Interfaces:**
- Consumes: `fleet_allow_at/remove/count`, `enroll_fp`, `fleet_db_rotate/save/epoch`, `enroll_open_window`, modal statics.
- Produces: `static void fleet_do_revoke(uint32_t now);`, `static void fleet_modal_touch(int px, int py, uint32_t now);` (consumed by Task 4).

- [ ] **Step 1: Add the revoke action.**

```c
static void fleet_do_revoke(uint32_t now){
    if (s_fleet_sel < 0 || s_fleet_sel >= (int)fleet_allow_count()) return;
    uint8_t idc[32]; memcpy(idc, fleet_allow_at(s_fleet_sel), 32);   // copy before swap-remove
    char fp[24]; enroll_fp(fp, sizeof fp, idc);
    fleet_allow_remove(idc);
    fleet_db_rotate();
    fleet_db_save();
    enroll_open_window(now);                     // survivors auto re-enroll to the new key
    ESP_LOGW(TAG, "enroll: REVOKED %s -> rotated to epoch %u, window reopened",
             fp, (unsigned)fleet_db_epoch());
    s_fleet_arm_ms = 0;
    s_fleet_modal = false;                       // auto-close so radar/enroll overlay is visible
    s_fleet_sel = 0; s_fleet_scroll = 0;
}
```

- [ ] **Step 2: Add the modal touch handler.**

```c
static void fleet_modal_touch(int px, int py, uint32_t now){
    int n = (int)fleet_allow_count();
    if (py >= FLEET_EXIT_Y){ s_fleet_modal = false; s_fleet_arm_ms = 0; return; }   // EXIT
    if (n > 0 && py >= FLEET_BTN_Y){                                                // button band
        if (px < 80){                                                              // UP
            if (s_fleet_sel > 0) s_fleet_sel--; s_fleet_arm_ms = 0;
        } else if (px < 160){                                                      // DOWN
            if (s_fleet_sel < n - 1) s_fleet_sel++; s_fleet_arm_ms = 0;
        } else {                                                                   // REVOKE
            if (s_fleet_arm_ms && (uint32_t)(now - s_fleet_arm_ms) < 3000) fleet_do_revoke(now);
            else s_fleet_arm_ms = now;                                             // arm
        }
        return;
    }
    if (n > 0 && py >= FLEET_ROW_Y0 && py < FLEET_ROW_Y0 + FLEET_ROWS_VISIBLE * FLEET_ROW_H){
        int idx = s_fleet_scroll + (py - FLEET_ROW_Y0) / FLEET_ROW_H;              // direct row select
        if (idx < n){ s_fleet_sel = idx; s_fleet_arm_ms = 0; }
    }
}
```

- [ ] **Step 3: Build to compile-verify.**

Run: `idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build`
Expected: compiles clean (still possibly `-Wunused-function` until Task 4).

- [ ] **Step 4: Commit.**

```
git add cyd/main/cyd_main.c
git commit -m "feat(fleet): revoke-modal input handler + revoke action (remove+rotate+reopen)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Integrate into the app_main loop + on-target verify

**Files:**
- Modify: `cyd/main/cyd_main.c` (the `for(;;)` render/touch loop in `app_main`, ~lines 548-637)

**Interfaces:**
- Consumes everything from Tasks 2-3.
- Produces: working end-to-end revoke UI.

- [ ] **Step 1: Define an always-valid `modal_open` flag and route touch to the modal when open.** `tx`/`ty` are already screen coords (the CONTROL-page zone checks compare them against `ty > 200`, `tx < 80`, etc.), so pass them straight through. Right after `bool edge = press && !was_press; was_press = press;` (line ~584), insert:

```c
#ifdef SIMULACRA_FLEET_PROVISION
        bool modal_open = s_fleet_modal;
        if (modal_open){
            if (edge){ fleet_modal_touch(tx, ty, now); radar_ui_note_input(&ui, now); }
        }
#else
        bool modal_open = false;
#endif
```

Then gate the two existing input blocks so they don't fire while the modal owns input:
- The long-press condition (line ~592) `if (press && !s_lp_fired && (now - s_press_start) >= 1500)` → add `&& !s_fleet_modal` (it's inside the fleet `#ifdef`, so `s_fleet_modal` is valid here).
- The general edge/view block (line ~600) `if (edge) {` → `if (edge && !modal_open) {` (uses the always-valid `modal_open`).

- [ ] **Step 2: Add the CONTROL-page entry hit-test.** Inside the existing `if (ui.view == RADAR_VIEW_CONTROL)` edge-handling block (under `#ifdef SIMULACRA_CONFIG_CTRL`), as the FIRST check (before the SEND/prev/next/center checks):

```c
#ifdef SIMULACRA_FLEET_PROVISION
                if (ty < 28){                        // top FLEET ROSTER bar
                    s_fleet_modal = true; s_fleet_sel = 0; s_fleet_scroll = 0; s_fleet_arm_ms = 0;
                    radar_ui_note_input(&ui, now);
                } else
#endif
```
(so it becomes `if (ty < 28){...} else if (ty > 200 && ...) { SEND } else if ...`).

- [ ] **Step 3: Render the modal (or the normal view + entry bar).** Inside the loop's `if (ui.backlight_on){ ... }` render block, replace the current tail:

```c
            radar_render_view(ui.view, &s_status, &lib, &ctrl, sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
#ifdef SIMULACRA_FLEET_PROVISION
            if (!draw_enroll_overlay(band, now))
#endif
            draw_freshness_overlay(band, now);
            sweep=(uint16_t)((sweep+12)%360);
```

with (the `#ifdef` wrapper keeps the gate-off build referencing only defined symbols — `draw_fleet_modal`/`draw_fleet_bar` exist only under the gate, and `modal_open` is constant-false there):

```c
#ifdef SIMULACRA_FLEET_PROVISION
            if (modal_open){
                draw_fleet_modal(band, now);
            } else
#endif
            {
                radar_render_view(ui.view, &s_status, &lib, &ctrl, sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
#ifdef SIMULACRA_FLEET_PROVISION
                bool enr = draw_enroll_overlay(band, now);
                if (enr)                                    { /* enrollment banner owns the top */ }
                else if (ui.view == RADAR_VIEW_CONTROL)     draw_fleet_bar(band);        // entry tab
                else                                        draw_freshness_overlay(band, now);
#else
                draw_freshness_overlay(band, now);
#endif
                sweep=(uint16_t)((sweep+12)%360);
            }
```

NOTE: no `goto` — the modal render stays inside the `if (ui.backlight_on)` guard, so nothing draws while the backlight is off. `modal_open` was computed once at the top of the loop (Step 1).

- [ ] **Step 4: Close the modal on backlight sleep.** In the `if (ui.backlight_on != bl_was_on)` transition block, when going to sleep, add:

```c
            if (!ui.backlight_on){ s_fleet_modal = false; s_fleet_arm_ms = 0; }
```

- [ ] **Step 5: Build clean (no unused-function warnings now).**

Run: `idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build`
Expected: builds; all modal functions referenced.

- [ ] **Step 6: Flash the CYD (production Vigil) and run the selftest build once to confirm Task 1.**

First the selftest build to see the new assertion pass:
```
idf.py fullclean
idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 -DFLEET_SELFTEST=1 -p COM14 flash
```
Capture serial: expect `fleet_db selftest: PASS (0 fails)`.

Then the clean production Vigil (removes FLEET_SELFTEST via fullclean):
```
idf.py fullclean
idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 -p COM14 flash
```
Expect boot: `fleetdb: loaded: epoch N, M allowed` (durable state) and `panel up`.

- [ ] **Step 7: On-target bench test (human at the panel).**
  1. Enroll a decoy if none present (C5 seek → CYD long-press → accept), or use the already-enrolled C5.
  2. CONTROL page → tap the top **FLEET ROSTER** bar → modal shows the decoy fingerprint; cross-check against the C5's serial print.
  3. Select it (row tap or UP/DOWN) → tap **REVOKE** → button shows `CONFIRM?` → tap again within 3 s.
  4. Confirm serial: `enroll: REVOKED <fp> -> rotated to epoch N+1, window reopened`, `fleetdb: saved`.
  5. Confirm the revoked C5 goes dark: its `status rx` stops (it re-requests but is never granted). Reboot CYD → `fleetdb: loaded: epoch N+1, 0 allowed` (durability).

- [ ] **Step 8: Commit + push + draft PR.**

```
git add cyd/main/cyd_main.c
git commit -m "feat(fleet): wire revoke modal into app loop (entry bar + input routing + render)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin feat/fleet-revoke-ui
gh pr create --draft --base main --head feat/fleet-revoke-ui --title "feat(fleet): revoke UI — on-panel roster + remove-then-rotate" --body "...🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

---

## Self-Review

- **Spec coverage:** modal surface (T2), entry bar (T2/T4), UP/DOWN/REVOKE arm-confirm/EXIT (T3), revoke = remove+rotate+save+reopen (T3), band-by-band render (T2), selftest (T1), HW test (T4). All spec sections mapped.
- **Type consistency:** `fleet_allow_at` returns `const uint8_t *`; copied into `idc[32]` before `fleet_allow_remove` (avoids swap-remove aliasing). `enroll_fp(out, cap, idpk)` signature matches existing use. Modal statics consistently named `s_fleet_*`.
- **Placeholder scan:** the only deliberate open item is the `tx/ty` vs `px/py` question in T4 Step 1, resolved inline (they are screen coords — call `fleet_modal_touch(tx, ty, now)`), and the `goto`-vs-flag choice, resolved with a stated preferred fallback. No TBDs.
