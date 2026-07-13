# CYD Necromancer Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the CYD (Vigil) front-end as a polished necromancer-themed dashboard — a sigil home with a live per-node fleet strip that navigates into reskinned detail views — on the existing `radar_ui`/`radar_render` framework.

**Architecture:** Extend three existing pieces, don't replace them. `radar_ui.c` (36-line view state machine) gains a HOME view and a tap-a-sigil→view / back→home navigation model. `radar_render.c` (110-line banded renderer) gains a `draw_home` and a theme reskin of the existing view drawers, all via the `radar_gfx` primitive set (rect/line/circle/text on a band buffer). `cyd/main/cyd_main.c` gains a per-node status table and home-grid touch hit-regions. One 3-byte wire field (shade-form counts) is added to surface Milestone A's RPA/NRPA/static breakdown.

**Tech Stack:** C (ESP-IDF, classic ESP32 + ILI9341 240×320 + XPT2046 resistive touch), `radar_gfx` band-buffer primitives (RGB565), MSVC `cl` host build for pure logic tests, on-target flash for the visual acceptance pass.

## Global Constraints

- **Public repo** — no absolute local paths, OS usernames, real hardware MACs, or real SSIDs in any committed file.
- **Renderer** — no full framebuffer; draw top-to-bottom in the reused band buffer via `radar_gfx`. Flat fills + hairline rules + primitive-drawn sigils only. **No LVGL.**
- **Type** — the panel font is the existing 8×8 bitmap (`radar_gfx_text`); serif small-caps in the mockup maps to this fixed font. Legibility over fidelity.
- **Touch** — resistive XPT2046, single-touch, no gestures; tap targets ≥ ~40px; navigation is taps + a back-target only.
- **Color** — RGB565. Palette lives as named `RGB565()` constants in `radar_theme.h`; no magic color literals in view code.
- **Reuse** — build on `radar_ui` / `radar_render` / `radar_gfx` / `fleet_db` / the signed status wire; do not fork them. Decoy (`main/`) changes limited to the shade-form status field + its `ble_devices` source.
- **Honest ceiling** — UI copy reports what the fleet does; no string overstates protection ("hidden" is vibe, not guarantee).
- **Commit trailer** — end every commit message with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`

---

## File Structure

- `components/simulacra_radar/radar_theme.h` (new) — RGB565 palette constants (void/crypt/edge/bone/ash/arcane/channel/ward/hunter) + `sigil_id_t`.
- `components/simulacra_radar/radar_sigil.{c,h}` (new) — `radar_sigil_draw(g, id, x, y, size, color)` draws the 6 occult glyphs from `radar_gfx` primitives.
- `components/simulacra_radar/radar_ui.{c,h}` (modify) — add `RADAR_VIEW_HOME`; add `radar_ui_select_view(ui, view, now)`; HOME is idle-default; wake-on-follower targets `RADAR_VIEW_RADAR` (Hunters).
- `components/simulacra_radar/radar_render.{c,h}` (modify) — add `draw_home`; add per-node fleet array + form-counts params to `radar_render_view`; reskin existing drawers to the palette; add `draw_info`.
- `components/simulacra_radar/radar_wire.h` (modify) — add `uint8_t form_restless, form_wandering, form_bound;` to `radar_wire_status_t`.
- `main/ble_devices.{c,h}` (modify) — add `ble_devices_form_counts(uint8_t *restless, uint8_t *wandering, uint8_t *bound)`.
- `main/webui.h` (modify) + decoy status fill (modify) — carry + populate the 3 form counts.
- `main/esp_now_link.c` (modify) — `espnow_status_from_webui` copies the 3 form counts.
- `cyd/main/cyd_main.c` (modify) — per-node status table (keyed by fleet member, stale-out), home-grid + back touch hit-regions, render-loop wiring, reskinned fleet modal.
- `cyd/main/fleet_status.{c,h}` (new) — pure per-node status table (upsert by node id, liveness/stale query); host-testable.
- Host tests under `tools/` — `radar_ui` nav tests, `fleet_status` table tests, `ble_devices_form_counts` test.

Note the render tasks give complete, compilable draw functions with real coordinates derived from the mockup; exact pixel positions are refined against the panel in Task 9 (on-target) — the code compiles and produces the layout as written.

---

## Task 1: Theme palette (RGB565 tokens)

**Files:**
- Create: `components/simulacra_radar/radar_theme.h`
- Test: `tools/radar_audit/test_theme.py` (+ a tiny host dumper) — OR fold into a compile check (see Step 2)

**Interfaces:**
- Produces: `RGB565(r,g,b)` macro and `COL_VOID/COL_CRYPT/COL_EDGE/COL_BONE/COL_ASH/COL_ARCANE/COL_CHANNEL/COL_WARD/COL_HUNTER` `uint16_t` constants; `sigil_id_t`.

- [ ] **Step 1: Write the header.** Create `components/simulacra_radar/radar_theme.h`:

```c
#pragma once
#include <stdint.h>

// RGB888 -> RGB565 (panel native). Constant-foldable so these are compile-time literals.
#define RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8) | (((g)&0xFC)<<3) | ((b)>>3)))

// Necromancer palette (see the design mockup). Named, never inline literals.
#define COL_VOID     RGB565(0x09,0x06,0x0F)   // ground
#define COL_CRYPT    RGB565(0x16,0x0E,0x24)   // panels / tiles
#define COL_EDGE     RGB565(0x2C,0x1E,0x45)   // hairline rules / rune borders
#define COL_BONE     RGB565(0xEC,0xE5,0xD4)   // primary text
#define COL_ASH      RGB565(0x85,0x79,0xA0)   // muted labels
#define COL_ARCANE   RGB565(0xA4,0x5C,0xF5)   // accent — sigils / selection
#define COL_CHANNEL  RGB565(0x4F,0xE0,0xB0)   // semantic: alive
#define COL_WARD     RGB565(0xE6,0xA6,0x4F)   // semantic: warning / dormant
#define COL_HUNTER   RGB565(0xF0,0x55,0x5F)   // semantic: detection

typedef enum {
    SIGIL_CIRCLE = 0,   // the fleet
    SIGIL_HUNTER,       // watching eye
    SIGIL_LIVING,       // heartbeat
    SIGIL_RITE,         // star
    SIGIL_WARD,         // key-eye
    SIGIL_GRIMOIRE,     // tome
    SIGIL_COUNT
} sigil_id_t;
```

- [ ] **Step 2: Verify the values compile + fold.** Add a throwaway host check (deleted after) or compile the component. Quick host verification:

Run:
```
python -c "def c(r,g,b): return ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3); print('%04X'%c(0xA4,0x5C,0xF5), '%04X'%c(0x4F,0xE0,0xB0))"
```
Expected: prints `A2FE 4F16` (arcane, channel) — confirms the macro math; if you change a hex, this is the reference.

- [ ] **Step 3: Commit.**
```bash
git add components/simulacra_radar/radar_theme.h
git commit -m "feat(cyd): necromancer RGB565 theme palette + sigil ids"
```

---

## Task 2: Sigil blitter (draw occult glyphs from primitives)

**Files:**
- Create: `components/simulacra_radar/radar_sigil.h`, `components/simulacra_radar/radar_sigil.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `radar_sigil.c` to SRCS)

**Interfaces:**
- Consumes: `radar_gfx_t` + primitives (`radar_gfx.h`), `sigil_id_t` (`radar_theme.h`).
- Produces: `void radar_sigil_draw(radar_gfx_t *g, sigil_id_t id, int cx, int cy, int r, uint16_t c);` — draws the glyph centered at (cx,cy) within radius r.

- [ ] **Step 1: Write the header.** Create `components/simulacra_radar/radar_sigil.h`:

```c
#pragma once
#include "radar_gfx.h"
#include "radar_theme.h"

// Draw sigil `id` centered at (cx,cy), fitting within radius r, in color c.
void radar_sigil_draw(radar_gfx_t *g, sigil_id_t id, int cx, int cy, int r, uint16_t c);
```

- [ ] **Step 2: Write the implementation.** Create `components/simulacra_radar/radar_sigil.c`:

```c
#include "radar_sigil.h"

static void star5(radar_gfx_t *g, int cx, int cy, int r, uint16_t c)
{
    // 5-point star via 5 outer points connected pentagram-style (indices step by 2).
    int px[5], py[5];
    // precomputed unit pentagon points (x1000), starting at top, clockwise
    static const int ux[5] = {0, 951, 588, -588, -951};
    static const int uy[5] = {-1000, -309, 809, 809, -309};
    for (int i = 0; i < 5; i++) { px[i] = cx + ux[i]*r/1000; py[i] = cy + uy[i]*r/1000; }
    for (int i = 0; i < 5; i++) radar_gfx_line(g, px[i], py[i], px[(i+2)%5], py[(i+2)%5], c);
}

void radar_sigil_draw(radar_gfx_t *g, sigil_id_t id, int cx, int cy, int r, uint16_t c)
{
    switch (id) {
    case SIGIL_CIRCLE:                       // ring + inscribed pentagram (the coven)
        radar_gfx_circle(g, cx, cy, r, c);
        star5(g, cx, cy, r - 2, c);
        break;
    case SIGIL_HUNTER: {                      // watching eye
        radar_gfx_line(g, cx-r, cy, cx, cy-r*3/5, c);
        radar_gfx_line(g, cx, cy-r*3/5, cx+r, cy, c);
        radar_gfx_line(g, cx+r, cy, cx, cy+r*3/5, c);
        radar_gfx_line(g, cx, cy+r*3/5, cx-r, cy, c);
        radar_gfx_circle(g, cx, cy, r/3, c);
        radar_gfx_fill_rect(g, cx-1, cy-1, 2, 2, c);
        break; }
    case SIGIL_LIVING: {                      // heartbeat
        radar_gfx_hline(g, cx-r, cx-r/3, cy, c);
        radar_gfx_line(g, cx-r/3, cy, cx-r/6, cy-r, c);
        radar_gfx_line(g, cx-r/6, cy-r, cx+r/8, cy+r, c);
        radar_gfx_line(g, cx+r/8, cy+r, cx+r/3, cy, c);
        radar_gfx_hline(g, cx+r/3, cx+r, cy, c);
        break; }
    case SIGIL_RITE:                          // star
        star5(g, cx, cy, r, c);
        radar_gfx_circle(g, cx, cy, r, c);
        break;
    case SIGIL_WARD:                          // key-eye (circle + shaft)
        radar_gfx_circle(g, cx-r/3, cy-r/3, r/2, c);
        radar_gfx_line(g, cx, cy, cx+r, cy+r, c);
        radar_gfx_line(g, cx+r*2/3, cy+r, cx+r, cy+r, c);
        break;
    case SIGIL_GRIMOIRE:                      // tome
        radar_gfx_fill_rect(g, cx-r, cy-r, 2, 2*r, c);        // spine
        radar_gfx_line(g, cx-r, cy-r, cx+r, cy-r, c);
        radar_gfx_line(g, cx-r, cy+r, cx+r, cy+r, c);
        radar_gfx_vline(g, cx+r, cy-r, cy+r, c);
        radar_gfx_hline(g, cx-r/2, cx+r/2, cy-r/3, c);
        radar_gfx_hline(g, cx-r/2, cx+r/2, cy, c);
        break;
    default: break;
    }
}
```

- [ ] **Step 3: Add to the component build.** In `components/simulacra_radar/CMakeLists.txt`, add `"radar_sigil.c"` to the `SRCS` list (next to `radar_render.c`/`radar_gfx.c`).

- [ ] **Step 4: Compile-check the component.** Build the CYD firmware skeleton far enough to compile the component (a full CYD build in Task 9; here a syntax/compile check):

Run: `grep -n "radar_sigil.c" components/simulacra_radar/CMakeLists.txt`
Expected: the source is listed. (Full compile is exercised by the first CYD build in Task 6/9; sigils use only existing `radar_gfx` primitives, so they compile once the component does.)

- [ ] **Step 5: Commit.**
```bash
git add components/simulacra_radar/radar_sigil.h components/simulacra_radar/radar_sigil.c components/simulacra_radar/CMakeLists.txt
git commit -m "feat(cyd): sigil blitter — draw the 6 occult glyphs from gfx primitives"
```

---

## Task 3: UI state — HOME view + tap-a-sigil navigation (TDD, host)

**Files:**
- Modify: `components/simulacra_radar/radar_ui.h`, `components/simulacra_radar/radar_ui.c`
- Create: `tools/radar_audit/run.ps1`, `tools/radar_audit/ui_dump.c`, `tools/radar_audit/tests/test_radar_ui.py`

**Interfaces:**
- Consumes: existing `radar_ui_t`, `radar_view_t`.
- Produces: `RADAR_VIEW_HOME` (new value 0, HOME first); `void radar_ui_select_view(radar_ui_t *ui, radar_view_t v, uint32_t now_ms);` (jump to a chosen view — replaces cycle-on-tap); `radar_ui_on_input` becomes "return to HOME"; idle-return targets HOME; wake-on-follower targets `RADAR_VIEW_RADAR`.

- [ ] **Step 1: Update the header.** In `components/simulacra_radar/radar_ui.h`, change the view enum so HOME is first and is the default/idle view, and declare the selector:

```c
typedef enum { RADAR_VIEW_HOME = 0, RADAR_VIEW_RADAR, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS,
               RADAR_VIEW_LIBRARY, RADAR_VIEW_CONTROL, RADAR_VIEW_INFO, RADAR_VIEW_COUNT } radar_view_t;

// Jump directly to a chosen view (home-grid tap). Wakes + resets timers.
void radar_ui_select_view(radar_ui_t *ui, radar_view_t v, uint32_t now_ms);
```
Keep the other declarations.

- [ ] **Step 2: Write the failing tests.** Create `tools/radar_audit/tests/test_radar_ui.py`:

```python
import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "ui_dump.exe" if os.name == "nt" else "ui_dump")

def run(script):
    # script: space-separated ops; ui_dump prints the view after each. See ui_dump.c.
    return subprocess.check_output([EXE] + script.split(), text=True).split()

@unittest.skipUnless(os.path.exists(EXE), "ui_dump not built")
class UI(unittest.TestCase):
    def test_home_is_default(self):
        self.assertEqual(run("reset")[0], "HOME")
    def test_select_jumps_to_view(self):
        # reset, select STATS(3), select HUNTERS(1)
        self.assertEqual(run("reset select 3 select 1"), ["HOME","STATS","RADAR"])
    def test_input_returns_home(self):
        self.assertEqual(run("reset select 4 input"), ["HOME","LIBRARY","HOME"])
    def test_idle_returns_home(self):
        # select STATS then tick after idle -> HOME
        self.assertEqual(run("reset select 3 idle_tick"), ["HOME","STATS","HOME"])
    def test_new_follower_wakes_to_hunters_when_idle(self):
        # on HOME, idle, a new follower -> RADAR (Hunters)
        self.assertEqual(run("reset idle follower1"), ["HOME","HOME","RADAR"])
```

- [ ] **Step 3: Write the host dumper.** Create `tools/radar_audit/ui_dump.c`:

```c
#include <stdio.h>
#include <string.h>
#include "radar_ui.h"
static const char *N[] = {"HOME","RADAR","DETAIL","STATS","LIBRARY","CONTROL","INFO"};
int main(int argc, char **argv){
    radar_ui_t ui; uint32_t t = 0; uint8_t threats = 0;
    for (int i = 1; i < argc; i++){
        if(!strcmp(argv[i],"reset")) radar_ui_reset(&ui,t,threats);
        else if(!strcmp(argv[i],"input")) radar_ui_on_input(&ui,t);
        else if(!strcmp(argv[i],"select")){ int v=atoi(argv[++i]); radar_ui_select_view(&ui,(radar_view_t)v,t); }
        else if(!strcmp(argv[i],"idle")){ t += RADAR_VIEW_IDLE_MS + 1; radar_ui_on_tick(&ui,t,threats); }
        else if(!strcmp(argv[i],"idle_tick")){ t += RADAR_VIEW_IDLE_MS + 1; radar_ui_on_tick(&ui,t,threats); }
        else if(!strcmp(argv[i],"follower1")){ threats = 1; radar_ui_on_tick(&ui,t,threats); }
        printf("%s\n", N[ui.view]);
    }
    return 0;
}
```

- [ ] **Step 4: Write the runner.** Create `tools/radar_audit/run.ps1`:

```powershell
$tool = $PSScriptRoot
$root = Join-Path $tool "..\.."
$rad  = Join-Path $root "components\simulacra_radar"
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /I $rad `
   (Join-Path $tool "ui_dump.c") (Join-Path $rad "radar_ui.c") /Fe:(Join-Path $tool "ui_dump.exe") | Out-Null
python -m unittest discover -s (Join-Path $tool "tests") -v
```

- [ ] **Step 5: Run — expect FAIL** (`radar_ui_select_view` undefined; input still cycles).

Run: `pwsh tools/radar_audit/run.ps1`
Expected: build error (missing `radar_ui_select_view`) or test failures. This confirms wiring.

- [ ] **Step 6: Implement the UI changes.** In `components/simulacra_radar/radar_ui.c`: set the reset default view to HOME; add the selector; make `radar_ui_on_input` return to HOME; retarget idle-return + wake to HOME/RADAR:

```c
void radar_ui_reset(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count)
{
    ui->view = RADAR_VIEW_HOME; ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms;
    ui->backlight_on = true; ui->last_threat_count = threat_count;
    ui->sel_preset = 0; ui->send_flash_ms = 0;
}
void radar_ui_select_view(radar_ui_t *ui, radar_view_t v, uint32_t now_ms)
{
    if (v < RADAR_VIEW_COUNT) ui->view = v;
    ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true;
}
void radar_ui_on_input(radar_ui_t *ui, uint32_t now_ms)   // back-target: return HOME
{
    ui->view = RADAR_VIEW_HOME;
    ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true;
}
```
And in `radar_ui_on_tick`, replace the two `RADAR_VIEW_RADAR` idle/wake targets: keep the **wake-on-new-follower** jump to `RADAR_VIEW_RADAR` (Hunters — an alert), but change the **idle-return** target to `RADAR_VIEW_HOME`:
```c
    // wake-on-follower: jump to Hunters (RADAR) only if the user is idle
    if (threat_count > ui->last_threat_count) {
        ui->backlight_on = true; ui->last_wake_ms = now_ms;
        if ((uint32_t)(now_ms - ui->last_input_ms) >= RADAR_VIEW_IDLE_MS)
            ui->view = RADAR_VIEW_RADAR;
    }
    ui->last_threat_count = threat_count;
    // idle-return to HOME
    if (ui->view != RADAR_VIEW_HOME &&
        (uint32_t)(now_ms - ui->last_input_ms) >= RADAR_VIEW_IDLE_MS) ui->view = RADAR_VIEW_HOME;
```
Leave the backlight logic unchanged.

- [ ] **Step 7: Run — expect PASS.**

Run: `pwsh tools/radar_audit/run.ps1`
Expected: all 5 `test_radar_ui.py` tests PASS.

- [ ] **Step 8: Commit.**
```bash
git add components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_ui.c tools/radar_audit
git commit -m "feat(cyd): HOME view + tap-a-sigil nav (idle->home, wake->hunters)"
```

---

## Task 4: Per-node fleet status table (TDD, host)

**Files:**
- Create: `cyd/main/fleet_status.h`, `cyd/main/fleet_status.c`
- Modify: `cyd/main/CMakeLists.txt` (add `fleet_status.c`)
- Test: `tools/radar_audit/tests/test_fleet_status.py` + `tools/radar_audit/fleet_dump.c`

**Interfaces:**
- Consumes: `radar_wire_status_t` (`radar_wire.h`).
- Produces: `fleet_status_t` (fixed table of up to `FLEET_STATUS_MAX` nodes); `fleet_status_reset`, `fleet_status_upsert(node_id, &st, now_ms)`, `fleet_status_count`, `fleet_status_at(i, &out_id, &out_st, &out_alive)`, using a `FLEET_STATUS_STALE_MS` liveness cutoff.

- [ ] **Step 1: Write the header.** Create `cyd/main/fleet_status.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "radar_wire.h"

#define FLEET_STATUS_MAX       4
#define FLEET_STATUS_STALE_MS  12000u   // no status this long -> node reads "silent"

typedef struct { uint8_t id; radar_wire_status_t st; uint32_t last_ms; bool used; } fleet_node_t;
typedef struct { fleet_node_t nodes[FLEET_STATUS_MAX]; } fleet_status_t;

void   fleet_status_reset(fleet_status_t *f);
void   fleet_status_upsert(fleet_status_t *f, uint8_t node_id, const radar_wire_status_t *st, uint32_t now_ms);
int    fleet_status_count(const fleet_status_t *f);                       // used slots
bool   fleet_status_at(const fleet_status_t *f, int i, uint8_t *id,
                       const radar_wire_status_t **st, bool *alive, uint32_t now_ms);
```

- [ ] **Step 2: Write the failing tests.** Create `tools/radar_audit/tests/test_fleet_status.py`:

```python
import os, subprocess, unittest
HERE=os.path.dirname(__file__); TOOL=os.path.dirname(HERE)
EXE=os.path.join(TOOL,"fleet_dump.exe" if os.name=="nt" else "fleet_dump")
def run(s): return subprocess.check_output([EXE]+s.split(),text=True).strip().splitlines()

@unittest.skipUnless(os.path.exists(EXE),"fleet_dump not built")
class FS(unittest.TestCase):
    def test_upsert_counts_distinct_nodes(self):
        # upsert node 5 (12 devices) then node 7 (8) -> count 2
        self.assertEqual(run("up 5 12 up 7 8 count"), ["2"])
    def test_upsert_same_node_updates_not_adds(self):
        self.assertEqual(run("up 5 12 up 5 16 count at0"), ["1","id=5 dev=16 alive=1"])
    def test_stale_node_reads_not_alive(self):
        # upsert node 5, advance past stale, query
        self.assertEqual(run("up 5 12 wait at0"), ["id=5 dev=12 alive=0"])
```

- [ ] **Step 3: Run — expect FAIL** (nothing built).

Run: `pwsh tools/radar_audit/run.ps1` (after Step 5 adds fleet sources to the runner)
Expected: FAIL (fleet_dump missing).

- [ ] **Step 4: Implement.** Create `cyd/main/fleet_status.c`:

```c
#include "fleet_status.h"
#include <string.h>

void fleet_status_reset(fleet_status_t *f){ memset(f, 0, sizeof(*f)); }

void fleet_status_upsert(fleet_status_t *f, uint8_t node_id, const radar_wire_status_t *st, uint32_t now_ms)
{
    int free_slot = -1;
    for (int i = 0; i < FLEET_STATUS_MAX; i++) {
        if (f->nodes[i].used && f->nodes[i].id == node_id) {
            f->nodes[i].st = *st; f->nodes[i].last_ms = now_ms; return;
        }
        if (!f->nodes[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        f->nodes[free_slot].used = true; f->nodes[free_slot].id = node_id;
        f->nodes[free_slot].st = *st; f->nodes[free_slot].last_ms = now_ms;
    }   // table full: drop (v1 fleet <= FLEET_STATUS_MAX)
}

int fleet_status_count(const fleet_status_t *f)
{ int n = 0; for (int i = 0; i < FLEET_STATUS_MAX; i++) if (f->nodes[i].used) n++; return n; }

bool fleet_status_at(const fleet_status_t *f, int i, uint8_t *id,
                     const radar_wire_status_t **st, bool *alive, uint32_t now_ms)
{
    int seen = 0;
    for (int k = 0; k < FLEET_STATUS_MAX; k++) {
        if (!f->nodes[k].used) continue;
        if (seen++ != i) continue;
        if (id) *id = f->nodes[k].id;
        if (st) *st = &f->nodes[k].st;
        if (alive) *alive = (uint32_t)(now_ms - f->nodes[k].last_ms) < FLEET_STATUS_STALE_MS;
        return true;
    }
    return false;
}
```

- [ ] **Step 5: Write the dumper + extend the runner.** Create `tools/radar_audit/fleet_dump.c`:

```c
#include <stdio.h>
#include <string.h>
#include "fleet_status.h"
int main(int argc,char**argv){
    fleet_status_t f; fleet_status_reset(&f); uint32_t t=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"up")){ uint8_t id=atoi(argv[++i]); radar_wire_status_t s; memset(&s,0,sizeof s);
            s.active_devices=atoi(argv[++i]); fleet_status_upsert(&f,id,&s,t); }
        else if(!strcmp(argv[i],"wait")){ t += FLEET_STATUS_STALE_MS + 1; }
        else if(!strcmp(argv[i],"count")) printf("%d\n", fleet_status_count(&f));
        else if(!strcmp(argv[i],"at0")){ uint8_t id; const radar_wire_status_t*st; bool a;
            if(fleet_status_at(&f,0,&id,&st,&a,t)) printf("id=%u dev=%u alive=%d\n",id,st->active_devices,a); }
    }
    return 0;
}
```
Extend `tools/radar_audit/run.ps1` to also build `fleet_dump.exe` (add a second `cl` line compiling `fleet_dump.c` + `cyd\main\fleet_status.c` with `/I ..\..\cyd\main /I <rad>`).

- [ ] **Step 6: Add to the CYD build.** In `cyd/main/CMakeLists.txt`, add `"fleet_status.c"` to `SRCS`.

- [ ] **Step 7: Run — expect PASS.**

Run: `pwsh tools/radar_audit/run.ps1`
Expected: `test_fleet_status.py` (3) + `test_radar_ui.py` (5) PASS.

- [ ] **Step 8: Commit.**
```bash
git add cyd/main/fleet_status.h cyd/main/fleet_status.c cyd/main/CMakeLists.txt tools/radar_audit
git commit -m "feat(cyd): per-node fleet status table (upsert + liveness stale-out)"
```

---

## Task 5: Shade-form wire field + decoy source (TDD, host)

**Files:**
- Modify: `components/simulacra_radar/radar_wire.h`, `main/webui.h`, `main/ble_devices.h`, `main/ble_devices.c`, `main/esp_now_link.c`, and the decoy status assembler (grep for where `webui_status_t.active_devices` is filled)
- Test: extend `tools/decoy_audit/tests/test_ble_devices.py`

**Interfaces:**
- Consumes: `ble_devices_at`, `ble_devices_count`, `ble_atype_t` (`ble_devices.h`).
- Produces: `void ble_devices_form_counts(uint8_t *restless, uint8_t *wandering, uint8_t *bound);` (restless=RPA, wandering=NRPA, bound=static); 3 `uint8_t` fields on `webui_status_t` and `radar_wire_status_t`.

- [ ] **Step 1: Write the failing test.** Append to `tools/decoy_audit/tests/test_ble_devices.py`:

```python
    def test_form_counts_sum_to_population(self):
        # a --formcounts subcommand prints "R W B N" (restless/wandering/bound/total)
        out = subprocess.check_output([EXE,"--formcounts","7","24"],text=True).split()
        r,w,b,n = map(int,out)
        self.assertEqual(r+w+b, n, "form counts must partition the population")
        self.assertEqual(n, 24)
```

- [ ] **Step 2: Add the dumper subcommand.** In `tools/decoy_audit/synth_dump.c`, add near the `--devices` block:

```c
    if (argc > 1 && strcmp(argv[1], "--formcounts") == 0) {
        srand(argc > 2 ? (unsigned)strtoul(argv[2],0,10) : 1);
        int n = argc > 3 ? (int)strtoul(argv[3],0,10) : 16;
        roster_init(); ble_devices_init(n, 0);
        uint8_t r=0,w=0,b=0; ble_devices_form_counts(&r,&w,&b);
        printf("%u %u %u %d\n", r, w, b, ble_devices_count());
        return 0;
    }
```

- [ ] **Step 3: Run — expect FAIL** (`ble_devices_form_counts` undefined).

Run: rebuild `synth_dump` (Task-1 cl line from the BLE plan) — expect a link/compile error for the missing symbol.

- [ ] **Step 4: Implement the counts.** In `main/ble_devices.h` declare, and in `main/ble_devices.c` add:

```c
void ble_devices_form_counts(uint8_t *restless, uint8_t *wandering, uint8_t *bound)
{
    uint8_t r=0,w=0,b=0;
    for (int i = 0; i < s_n; i++) {
        if (!s_dev[i].alive) continue;
        switch (s_dev[i].atype) {
            case BLE_ATYPE_RPA:  r++; break;   // restless (rotating)
            case BLE_ATYPE_NRPA: w++; break;   // wandering
            default:             b++; break;   // static -> bound
        }
    }
    if (restless) *restless = r; if (wandering) *wandering = w; if (bound) *bound = b;
}
```

- [ ] **Step 5: Run — expect PASS.**

Run: `pwsh tools/decoy_audit/run.ps1 -Rebuild` (or the cl rebuild) then the unittest.
Expected: `test_form_counts_sum_to_population` PASS; all prior BLE tests still PASS.

- [ ] **Step 6: Thread the field through the wire.** Add `uint8_t form_restless, form_wandering, form_bound;` to `radar_wire_status_t` (`radar_wire.h`) and to `webui_status_t` (`main/webui.h`). In `main/esp_now_link.c`, in `espnow_status_from_webui`, copy the three fields (`out->form_restless = in->form_restless;` etc.). In the decoy status assembler (grep `active_devices =` under `main/` to find where `webui_status_t` is populated — likely `webui.c`/`coexist.c`), populate them:
```c
    ble_devices_form_counts(&st.form_restless, &st.form_wandering, &st.form_bound);
```

- [ ] **Step 7: Build both decoy targets** (fleet flags per the BLE plan) to confirm the wire threading compiles on C5 and C6.

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Do build` then `-Target c6 -Do build`
Expected: both `Project build complete`.

- [ ] **Step 8: Commit.**
```bash
git add components/simulacra_radar/radar_wire.h main/webui.h main/ble_devices.h main/ble_devices.c main/esp_now_link.c main/webui.c main/coexist.c tools/decoy_audit
git commit -m "feat(ble): shade-form counts (RPA/NRPA/static) on the status wire"
```

---

## Task 6: Render — HOME screen + theme reskin

**Files:**
- Modify: `components/simulacra_radar/radar_render.h`, `components/simulacra_radar/radar_render.c`

**Interfaces:**
- Consumes: `radar_theme.h`, `radar_sigil.h`, `radar_gfx.h`, `fleet_status_t` view (passed as arrays), `radar_wire_status_t`.
- Produces: `radar_render_view` extended to accept the per-node fleet array; a `draw_home` internal; reskinned view drawers; a `draw_info` internal.

- [ ] **Step 1: Extend the render signature.** In `radar_render.h`, add a fleet-nodes view struct and extend `radar_render_view`:

```c
typedef struct { uint8_t id; const radar_wire_status_t *st; bool alive; } radar_node_view_t;

void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_node_view_t *nodes, int node_count,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
```

- [ ] **Step 2: Implement `draw_home`.** In `radar_render.c` (include `radar_theme.h`, `radar_sigil.h`), add a home drawer that renders per band. The banded renderer calls the drawer once per band with `g->y0` set; draw only what intersects the band (the existing drawers already do this pattern — follow it). Home layout (240×320): top bar 0–26, fleet strip 26–104, sigil grid 104–298 (2 cols × 3 rows), ticker 298–320:

```c
static void hb_text_center(radar_gfx_t *g, int cx, int y, const char *s, uint16_t c){
    int w = (int)strlen(s) * 8; radar_gfx_text(g, cx - w/2, y, s, c);
}
static void draw_home(radar_gfx_t *g, const radar_node_view_t *nodes, int nc)
{
    radar_gfx_clear(g, COL_VOID);
    // top bar
    radar_gfx_fill_rect(g, 0, 0, 240, 26, COL_CRYPT);
    radar_gfx_hline(g, 0, 239, 26, COL_EDGE);
    radar_sigil_draw(g, SIGIL_CIRCLE, 12, 13, 7, COL_ARCANE);
    radar_gfx_text(g, 26, 9, "SIMULACRA", COL_BONE);
    // fleet strip: up to 3 node cards across
    int cols = nc < 1 ? 1 : (nc > 3 ? 3 : nc);
    for (int i = 0; i < cols; i++) {
        int x = i * 80, y = 30;
        radar_gfx_fill_rect(g, x+2, y, 76, 70, COL_CRYPT);
        uint16_t sc = nodes[i].alive ? COL_CHANNEL : COL_ASH;
        char name[8]; snprintf(name, sizeof name, "N%u", nodes[i].id);
        radar_gfx_text(g, x+8, y+6, name, COL_BONE);
        radar_gfx_fill_rect(g, x+68, y+7, 4, 4, sc);                 // pulse dot
        char num[8]; snprintf(num, sizeof num, "%u", nodes[i].alive ? nodes[i].st->active_devices : 0);
        radar_gfx_text(g, x+8, y+24, num, COL_BONE);
        radar_gfx_text(g, x+8, y+50, nodes[i].alive ? "CHANNEL" : "SILENT", sc);
    }
    // sigil grid 2x3
    static const sigil_id_t sig[6] = {SIGIL_CIRCLE,SIGIL_HUNTER,SIGIL_LIVING,SIGIL_RITE,SIGIL_WARD,SIGIL_GRIMOIRE};
    static const char *lbl[6] = {"CIRCLE","HUNTERS","LIVING","RITES","WARDS","GRIMOIRE"};
    for (int i = 0; i < 6; i++) {
        int cx = (i % 2) * 120, cy = 104 + (i / 2) * 64;
        radar_gfx_fill_rect(g, cx+1, cy+1, 118, 62, COL_CRYPT);
        radar_sigil_draw(g, sig[i], cx+22, cy+31, 12, COL_ARCANE);
        radar_gfx_text(g, cx+42, cy+27, lbl[i], COL_BONE);
    }
    // ticker
    radar_gfx_hline(g, 0, 239, 298, COL_EDGE);
    radar_gfx_text(g, 6, 304, "THE CIRCLE CHANNELS", COL_ASH);
}
```
Note: `draw_home` clears+draws the full frame each band-call as the existing drawers do (the band buffer clips writes to its rows via `radar_gfx_pixel`); if the existing renderer instead calls the drawer once with a full-height band, keep the same convention as `draw_radar`. Match `draw_radar`'s exact banding contract when wiring (Step 4).

- [ ] **Step 3: Reskin existing drawers + add `draw_info`.** Replace the existing hardcoded colors in `draw_radar` and the other view drawers with the `COL_*` tokens (bone text, arcane rings, hunter dots for threats, void ground). Add a minimal `draw_info` (firmware version string, fleet epoch from `st->epoch`, node count) using the same primitives. Keep each drawer's structure; only swap palette + add the DETAIL form-breakdown line:
```c
    // in the DETAIL drawer, under the shades total:
    char forms[40];
    snprintf(forms, sizeof forms, "%u REST %u WAND %u BND",
             st->form_restless, st->form_wandering, st->form_bound);
    radar_gfx_text(g, 10, <y>, forms, COL_ASH);
```

- [ ] **Step 4: Wire the dispatch.** In `radar_render_view`, add `case RADAR_VIEW_HOME: draw_home(&g, nodes, node_count); break;` and `case RADAR_VIEW_INFO: draw_info(&g, st); break;`, following the exact per-band call structure already in the function.

- [ ] **Step 5: Compile the CYD firmware.** (First full CYD build.)

Run: build the CYD app (see Task 9 for the exact CYD build command).
Expected: `Project build complete`; no unused-var/format warnings from the reskin.

- [ ] **Step 6: Commit.**
```bash
git add components/simulacra_radar/radar_render.h components/simulacra_radar/radar_render.c
git commit -m "feat(cyd): HOME render + theme reskin + shade-form breakdown"
```

---

## Task 7: CYD integration — touch regions + per-node wiring

**Files:**
- Modify: `cyd/main/cyd_main.c`

**Interfaces:**
- Consumes: `fleet_status_*` (Task 4), `radar_ui_select_view` (Task 3), `radar_render_view` new signature (Task 6).

- [ ] **Step 1: Track per-node status.** Where `cyd_main.c` receives a decoy status (the ESP-NOW status handler that currently sets `s_status`), also `fleet_status_upsert(&s_fleet, node_id, &incoming, now)` keyed by the sender's fleet id. Keep `s_status` as the "focused node" for the DETAIL view. Add `static fleet_status_t s_fleet;` and `fleet_status_reset(&s_fleet)` at init.

- [ ] **Step 2: Build the node-view array for render.** Before each render, populate a `radar_node_view_t nv[FLEET_STATUS_MAX]`:
```c
    radar_node_view_t nv[FLEET_STATUS_MAX]; int nc = 0;
    for (int i = 0; i < FLEET_STATUS_MAX; i++) {
        uint8_t id; const radar_wire_status_t *st; bool alive;
        if (!fleet_status_at(&s_fleet, i, &id, &st, &alive, now)) break;
        nv[nc].id = id; nv[nc].st = st; nv[nc].alive = alive; nc++;
    }
    radar_render_view(ui.view, &s_status, nv, nc, /*lib*/l, /*ctrl*/c, sweep, band, BAND_H, LCD_W, LCD_H, flush, &fctx);
```

- [ ] **Step 3: Map home-grid touches.** In the touch handler, when `ui.view == RADAR_VIEW_HOME`, hit-test the 6 sigil tiles (2 cols × 3 rows, origin y=104, tile 120×64) and the 3 fleet cards (y 30–100, 80px cols → DETAIL for that node), and call `radar_ui_select_view` with the mapped view; otherwise (non-home views) a touch calls `radar_ui_on_input` (back to HOME). Example region math:
```c
    if (ui.view == RADAR_VIEW_HOME) {
        if (py >= 104) { int col = px / 120, row = (py - 104) / 64;
            int idx = row * 2 + col;
            static const radar_view_t map[6] = {RADAR_VIEW_DETAIL, RADAR_VIEW_RADAR, RADAR_VIEW_STATS,
                                                RADAR_VIEW_CONTROL, RADAR_VIEW_LIBRARY, RADAR_VIEW_INFO};
            if (idx >= 0 && idx < 6) radar_ui_select_view(&ui, map[idx], now);
        } else if (py >= 30 && py < 100) {   // fleet card -> focus that node's DETAIL
            int ni = px / 80; /* set s_status focus to nv[ni] */ radar_ui_select_view(&ui, RADAR_VIEW_DETAIL, now);
        }
    } else {
        radar_ui_on_input(&ui, now);   // any touch off-home returns home
    }
```
(Wire the CONTROL page's existing preset-tap handling to the trimmed Rites page — Dormant/Wake — in the reskin; drop the dead-preset cycling.)

- [ ] **Step 4: Build + confirm no touch dead-zones.** Full CYD build; verify compile.

Run: CYD build command (Task 9).
Expected: `Project build complete`.

- [ ] **Step 5: Commit.**
```bash
git add cyd/main/cyd_main.c
git commit -m "feat(cyd): per-node status wiring + home-grid touch regions"
```

---

## Task 8: Trim the Rites (CONTROL) page + reskin the fleet modal

**Files:**
- Modify: `cyd/main/cyd_main.c`, `components/simulacra_radar/radar_render.c`

- [ ] **Step 1: Trim CONTROL to Dormant/Wake.** The density/timing presets are inert (BLE milestone). In the CONTROL drawer + its touch handling, replace the 5-preset selector with a single **Dormant / Wake** toggle (sends `SIM_PRESET_PAUSE` / `SIM_PRESET_NORMAL` — the surviving live control) and an "auto-density" status line. Remove the STEALTH/DENSE/MAX preset tiles.

- [ ] **Step 2: Reskin the fleet modal.** Apply `COL_*` tokens to `draw_fleet_modal`/`draw_fleet_bar`/`draw_btn` so the enrolment/revoke UI matches the theme (arcane borders, bone text, hunter for REVOKE-armed).

- [ ] **Step 3: Build.** Full CYD build → `Project build complete`.

- [ ] **Step 4: Commit.**
```bash
git add cyd/main/cyd_main.c components/simulacra_radar/radar_render.c
git commit -m "feat(cyd): trim Rites to Dormant/Wake; reskin fleet modal"
```

---

## Task 9: On-target acceptance (hardware)

- [ ] **Step 1: Flash the CYD.** Build + flash the CYD firmware (classic ESP32, ILI9341). Use the CYD build/flash path (the project builds the CYD app under `cyd/`; flash via `idf.py -C cyd -p COM17 flash` with the classic-ESP32 IDF, or the documented CYD flow). Expected: `Hash of data verified`.

- [ ] **Step 2: Flash the decoys** with the shade-form field build (fleet flags, C5 + C6) so real per-node status + form counts flow.

- [ ] **Step 3: Acceptance checklist (visual, on the panel):**
  - Home renders: wordmark, fleet strip with per-node shade counts + channeling/silent state, 6 sigil tiles, ticker.
  - Tapping a sigil opens its view; tapping off-home returns to Home; idle (~15 s) returns to Home; backlight idles off when clear.
  - Fleet strip shows **live** counts from the decoys and a node greys to "SILENT" when unplugged (stale-out).
  - DETAIL shows the shade-form breakdown (REST/WAND/BND) from a real decoy.
  - A live follower wakes the screen to Hunters.
  - Rites toggles Dormant/Wake and the decoy actually pauses/resumes.
  - Record the result (and a photo for the DEFCON deck) in `private/`.

- [ ] **Step 4: Commit** any pixel-position tuning made during Step 3 (coordinates refined against the panel).
```bash
git add -A && git commit -m "fix(cyd): on-panel layout tuning"
```

---

## Task 10: Finish the branch

- [ ] **Step 1:** Host suite green (`pwsh tools/radar_audit/run.ps1` + `pwsh tools/decoy_audit/run.ps1 -Rebuild`), CYD + both decoys flashed clean.
- [ ] **Step 2:** Note the deferred items (Grimoire full logs, fleet-strip paging past 3 nodes, auto-density automatic reprofile) in `private/FUTURE-FEATURES.md`.
- [ ] **Step 3:** Use superpowers:finishing-a-development-branch to verify tests, present merge/PR options, and complete the branch.

---

## Self-Review

- **Spec coverage:** §1 goal (sigil home + fleet strip) → Tasks 6/7. §3 architecture (extend radar_ui/render/gfx; per-node table) → Tasks 3/4/6/7. §4 views (HOME/Hunters/Living/Rites/Wards/Grimoire-INFO) → Tasks 3 (enum), 6 (render), 7 (nav), 8 (Rites). §4 shade-form field → Task 5. §5 theme tokens → Task 1. §5 sigils → Task 2. §7 testing (UI-state, per-node table, form counts host tests; on-target acceptance) → Tasks 3/4/5/9. §8 non-goals honored (no LVGL, no new controls beyond Dormant/Wake, only the form wire field, minimal animation, no gestures, INFO stub). §9 constraints + honest ceiling → Global Constraints. §10 locked decisions (form field, wake→Hunters, INFO stub) → Tasks 5/3/6. Covered.
- **Placeholder scan:** logic/data tasks (1–5) carry complete code + host tests. Render/integration tasks (6–8) carry complete draw/wiring functions with real coordinates + palette; Task-6 Step-2 notes the drawers must match `draw_radar`'s exact band-call contract (verified when wiring) and Task-9 tunes pixel positions on-panel — this is inherent to graphics-on-hardware, not a deferred placeholder. No "TBD"/"handle later".
- **Type consistency:** `radar_view_t` values (HOME first), `radar_ui_select_view(ui,view,now)`, `fleet_status_*` signatures, `radar_node_view_t {id,st,alive}`, `ble_devices_form_counts(r,w,b)`, and the 3 wire fields `form_restless/wandering/bound` are used identically across the header, host dumpers, render, and CYD wiring. `radar_render_view`'s new `nodes,node_count` params are threaded in Tasks 6 (def) and 7 (call).
- **Ordering:** theme+sigils (1–2) precede render (6); UI-state (3) + per-node table (4) precede CYD wiring (7); the wire field (5) precedes DETAIL's form line (6) and on-target (9). Host-testable logic (1–5) lands before the hardware-only render/integration (6–9).
