# Ward Display Console — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Ward (ESP32-C5) decoy an SBT240 (ILI9341/XPT2046/microSD) screen that renders its own status + fleet radar, lets touch adjust **Ward's own** churn density, and persists the learned-template DB to SD — with **no fleet-control authority** (no private key on Ward).

**Architecture:** A presentation + local-control + SD-persistence layer added to the `main/` decoy firmware behind `#if SIMULACRA_DISPLAY`. It reuses the shared renderer (`radar_render_view`), the runtime settings backend (`sim_settings_*`), and the sealed learned-DB codec (`learn_db_seal/open`). The only genuinely new code is a C5 display/touch/SD HAL and a centralized pin map. It ports directly from the CYD/Vigil implementation in `cyd/main/cyd_main.c`.

**Tech Stack:** ESP-IDF v5.5 (esp32c5), `esp_lcd` + `esp_lcd_ili9341` managed component, `sdspi`/`fatfs`, bit-banged XPT2046, C (C11).

**Spec:** `docs/design/specs/2026-07-08-ward-display-console-design.md`

## Global Constraints

- **Build/flash (Ward):** source `~/esp/v5.5/esp-idf/export.ps1` in every shell. The decoy root `sdkconfig` is shared across build dirs, so when switching from a C6 build run `idf.py -B build_c5 set-target esp32c5` first. Build with:
  `idf.py -B build_c5 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_DISPLAY=1 build`
- **Selftests run on-target.** There is no host test runner; selftests compile into the firmware behind `-DCHURN_SELFTEST=1` and print `selftest: <total>/<total> pass` (or `FAIL: <msg>`) over serial at boot via the `ST_CHECK` macro in `main/churn_selftest.c`. "Run the test" = build with `-DCHURN_SELFTEST=1`, flash, read the serial line. Pure-logic tasks get `ST_CHECK` tests; display/touch/SD bring-up tasks get on-target visual/serial checks (they cannot be unit-tested off-hardware).
- **Port before flashing:** always `python -m esptool --port COMx chip_id` to confirm the C5 port; do not trust remembered COM numbers.
- **`-D` gate flags** only reach the compiler through the `foreach` forwarder in `main/CMakeLists.txt`. Any new gate MUST be added there.
- **No private key on Ward.** `cyd/main/sim_ctrl_sk.h` and TweetNaCl signing stay CYD-only. Ward never gains an ESP-NOW **send** path for `RADAR_TYPE_CONFIG`.
- **Persona sizing** stays `#if CONFIG_IDF_TARGET_ESP32C5` (Ward = C5, `CHURN_ACTIVE_SET` ceiling 8). Each persona-split file must `#include "sdkconfig.h"` first (project gotcha).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## File Structure

- **Create `main/ward_pins.h`** — every display/touch/SD GPIO + SPI host id as `#define`s. The single source of truth for wiring; the only file that changes if the pin map moves.
- **Create `main/ward_display.{c,h}`** — C5 display HAL (SPI2 + `esp_lcd_ili9341` + backlight), touch init/read (bit-banged XPT2046), the render/touch loop task, and view dispatch. Public entry `ward_display_start()`.
- **Create `main/ward_librarian.{c,h}`** — SD mount, `learn_db` seal/open persistence of the RAM learned set, boot restore, throttled atomic save. Public `ward_librarian_init()` / `ward_librarian_tick()`.
- **Create `main/idf_component.yml`** — declares the `espressif/esp_lcd_ili9341` managed dependency (mirrors `cyd/main/idf_component.yml`).
- **Modify `main/CMakeLists.txt`** — add the new sources, add `SIMULACRA_DISPLAY` to the gate forwarder, add `esp_lcd fatfs sdmmc esp_timer` to `REQUIRES`.
- **Modify `main/simulacra_main.c`** — call `ward_display_start()` + `ward_librarian_init()` behind `#if SIMULACRA_DISPLAY`.
- **Modify `main/churn_selftest.c`** — add Ward preset-mapping + librarian round-trip tests.
- **Modify `sdkconfig.defaults`** — FATFS LFN settings for the C5 build.

---

### Task 1: Pin map + build gate scaffolding

**Files:**
- Create: `main/ward_pins.h`
- Modify: `main/CMakeLists.txt` (gate forwarder + REQUIRES)

**Interfaces:**
- Produces: `WARD_SPI_HOST`, `WARD_PIN_SCK/MOSI/MISO/DISP_CS/DC/RST/BL/SD_CS`, `WARD_PIN_T_CLK/T_CS/T_DIN/T_DOUT/T_IRQ`, `WARD_LCD_W`(240)/`WARD_LCD_H`(320) — consumed by Tasks 2, 4, 6.

- [ ] **Step 1: Bench-confirm two caveats before soldering** (the pin map itself is already resolved to the ESP32-C5-WIFI6-KIT-N16R8 exposed header — see `docs/hardware/ward-display-build.md` notes / the pinout images): (a) GPIO0/1 have **no 32.768 kHz crystal** populated (none visible in board photos), and (b) the JTAG pads GPIO3/4/5 won't be needed for a hardware debugger during use. If (a) fails, move `T_CLK`/`T_CS` to GPIO25/26 (strap pins, output-only OK) or reclaim GPIO13. This is a hardware read, not a code step.

- [ ] **Step 2: Write `main/ward_pins.h`** with the confirmed assignment:

```c
#pragma once
#include "sdkconfig.h"
#include "driver/spi_common.h"

// Ward (ESP32-C5-WIFI6-KIT-N16R8, ESP32-C5-WROOM-1) <-> SBT240-W61 (ILI9341 + XPT2046 + SD).
// Exposed header GPIOs: {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,23,24,25,26,27,28}.
// GPIO16-22 are NOT on the header (internal to the N16R8 flash/PSRAM).
// Off-limits: strapping 2/7/25/26/27(RGB)/28, UART0 console 11/12, native-USB 13/14.
#define WARD_LCD_W        240
#define WARD_LCD_H        320

#define WARD_SPI_HOST     SPI2_HOST      // display + SD share this bus (separate CS)
#define WARD_PIN_SCK      6
#define WARD_PIN_MOSI     23
#define WARD_PIN_MISO     24             // SD read-back (display is write-only)
#define WARD_PIN_DISP_CS  15
#define WARD_PIN_DC       8
#define WARD_PIN_RST      3              // MTDI/JTAG pad, output-only -> OK post-boot
#define WARD_PIN_SD_CS    9
#define WARD_PIN_BL       (-1)           // backlight tied to 3V3 (Ward is powered); no GPIO

// Touch: bit-banged on its own pins (no SPI host needed).
#define WARD_PIN_T_CLK    0              // out (verify no 32.768kHz xtal on GPIO0/1)
#define WARD_PIN_T_CS     1              // out
#define WARD_PIN_T_DIN    4              // out (MTCK/JTAG)
#define WARD_PIN_T_DOUT   5              // in  (MTDO/JTAG)
#define WARD_PIN_T_IRQ    10             // in
```

- [ ] **Step 3: Add `SIMULACRA_DISPLAY` to the gate forwarder** in `main/CMakeLists.txt` — append it to the `foreach(flag ...)` list (after `SIMULACRA_CONFIG_CTRL`).

- [ ] **Step 4: Add components to `REQUIRES`** in `main/CMakeLists.txt`: append `esp_lcd fatfs sdmmc esp_timer` to the existing `REQUIRES` line.

- [ ] **Step 5: Verify the gate compiles (no-op yet).** Build headless to prove nothing broke:
  `idf.py -B build_c5 set-target esp32c5 && idf.py -B build_c5 -DSIMULACRA_DISPLAY=1 build`
  Expected: build succeeds (the flag is defined but unused so far).

- [ ] **Step 6: Commit.**
```bash
git add main/ward_pins.h main/CMakeLists.txt
git commit -m "feat(ward): pin map header + SIMULACRA_DISPLAY build gate"
```

---

### Task 2: C5 display HAL — bring up the panel

**Files:**
- Create: `main/ward_display.c`, `main/ward_display.h`
- Create: `main/idf_component.yml`
- Modify: `main/CMakeLists.txt` (add `ward_display.c` to SRCS)

**Interfaces:**
- Consumes: `main/ward_pins.h`.
- Produces: `bool ward_display_hw_init(void)` (inits SPI2 bus, ILI9341 panel, backlight; returns true on success), `void ward_flush(int y0, int h, const uint16_t *buf, void *ctx)` (a `radar_flush_fn`), `void ward_backlight(bool on)`. Consumed by Tasks 3, 4, 6.

- [ ] **Step 1: Create `main/idf_component.yml`** mirroring the CYD's:
```yaml
dependencies:
  espressif/esp_lcd_ili9341: "^1"
```

- [ ] **Step 2: Write `main/ward_display.h`:**
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>
bool ward_display_hw_init(void);                                  // SPI2 + ILI9341 + backlight
void ward_flush(int y0, int h, const uint16_t *buf, void *ctx);   // radar_flush_fn
void ward_backlight(bool on);
```

- [ ] **Step 3: Write `main/ward_display.c` HAL** (ported from `cyd_panel_init`/`cyd_flush`, but the SPI2 bus config includes MISO so SD can share it, and backlight is a plain GPIO unless a PWM pin is confirmed):
```c
#include "ward_display.h"
#include "ward_pins.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "ward.disp";
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static uint16_t s_txbuf[WARD_LCD_W * 40];   // byte-swap scratch, >= one band

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx){
    (void)io;(void)e;(void)ctx; BaseType_t hp=pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done,&hp); return hp==pdTRUE;
}

void ward_flush(int y0, int h, const uint16_t *buf, void *ctx){
    (void)ctx; int n = WARD_LCD_W * h;
    for (int i=0;i<n;i++) s_txbuf[i] = __builtin_bswap16(buf[i]);   // ILI9341 wants BE RGB565
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, WARD_LCD_W, y0+h, s_txbuf);
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
}

void ward_backlight(bool on){ if (WARD_PIN_BL >= 0) gpio_set_level(WARD_PIN_BL, on?1:0); }

bool ward_display_hw_init(void){
    s_flush_done = xSemaphoreCreateBinary();
    if (WARD_PIN_BL >= 0) {   // backlight is tied to 3V3 on this board (BL == -1) -> no-op
        gpio_config_t bl = { .pin_bit_mask=1ULL<<WARD_PIN_BL, .mode=GPIO_MODE_OUTPUT };
        gpio_config(&bl); ward_backlight(true);
    }

    spi_bus_config_t bus = { .mosi_io_num=WARD_PIN_MOSI, .sclk_io_num=WARD_PIN_SCK,
        .miso_io_num=WARD_PIN_MISO, .quadwp_io_num=-1, .quadhd_io_num=-1,
        .max_transfer_sz=WARD_LCD_W*40*2+8 };
    if (spi_bus_initialize(WARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO)!=ESP_OK) return false;

    esp_lcd_panel_io_handle_t io=NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = { .dc_gpio_num=WARD_PIN_DC, .cs_gpio_num=WARD_PIN_DISP_CS,
        .pclk_hz=40*1000*1000, .lcd_cmd_bits=8, .lcd_param_bits=8, .spi_mode=0,
        .trans_queue_depth=10, .on_color_trans_done=on_trans_done };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)WARD_SPI_HOST,&io_cfg,&io)!=ESP_OK) return false;
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num=WARD_PIN_RST,
        .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel=16 };
    if (esp_lcd_new_panel_ili9341(io,&pc,&s_panel)!=ESP_OK) return false;
    esp_lcd_panel_reset(s_panel); esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel,false);     // flip at bring-up if colors invert
    esp_lcd_panel_mirror(s_panel,true,false);      // adjust at bring-up for text orientation
    esp_lcd_panel_disp_on_off(s_panel,true);
    ESP_LOGW(TAG,"ILI9341 up on SPI2"); return true;
}
```

- [ ] **Step 4: Add `ward_display.c` to `main/CMakeLists.txt` SRCS.**

- [ ] **Step 5: Temporary bring-up test — fill the screen.** In `simulacra_main.c`, behind `#if SIMULACRA_DISPLAY`, before the normal init, add a throwaway: `ward_display_hw_init();` then flush two solid bands (e.g. a red band and a blue band) using `ward_flush`. Build + flash:
  `idf.py -B build_c5 -DSIMULACRA_DISPLAY=1 -p COMx flash monitor`
  Expected serial `ILI9341 up on SPI2`; screen shows the two color bands the right way up. Note any color-inversion/mirroring to fix in Step 3's flags. **Remove the throwaway before commit.**

- [ ] **Step 6: Commit.**
```bash
git add main/ward_display.c main/ward_display.h main/idf_component.yml main/CMakeLists.txt
git commit -m "feat(ward): C5 ILI9341 display HAL over SPI2"
```

---

### Task 3: Render the radar/status view on Ward

**Files:**
- Modify: `main/ward_display.c` (add render loop + status source), `main/ward_display.h` (add `ward_display_start`)
- Modify: `main/simulacra_main.c` (call `ward_display_start()`)

**Interfaces:**
- Consumes: `radar_render_view(view, st, lib, lib_n, ctrl, band, band_h, w, h, flush, ctx)` from `radar_render.h`; `radar_ui_*` from `radar_ui.h`.
- Produces: `void ward_display_start(void)` — spawns the render/touch task. Consumed by Task 4/5 (extends the same loop) and `simulacra_main.c`.

- [ ] **Step 1: Build Ward's own status snapshot.** Ward is a decoy, not a radar client, so populate a local `radar_wire_status_t` from its own churn state each frame (phantom/active count, threat count) rather than from ESP-NOW. Add a small helper `static void ward_fill_status(radar_wire_status_t *st)` using `churn_active_count()` and the detect/threat counters already used by the web UI status path (`webui.c` shows the field mapping to copy).

- [ ] **Step 2: Write the render/touch task** in `ward_display.c`:
```c
void ward_display_start(void){
    xTaskCreate(ward_task, "ward_disp", 4096, NULL, 3 /*low prio*/, NULL);
}
```
where `ward_task` mirrors the CYD `app_main` loop: init `radar_ui_t`, then each iteration fill status, call `radar_render_view(ui.view, &st, NULL, 0, &ctrl_state, band, 40, WARD_LCD_W, WARD_LCD_H, ward_flush, NULL)`, `radar_ui_on_tick`, and `vTaskDelay(pdMS_TO_TICKS(100))` (≈10 fps, low priority so radios never starve). Touch handling is added in Task 4; for now start on `RADAR_VIEW_RADAR`.

- [ ] **Step 3: Hook into boot.** In `simulacra_main.c`, after the existing decoy init, add:
```c
#if SIMULACRA_DISPLAY
    ward_display_start();
#endif
```
(with `#include "ward_display.h"` guarded the same way).

- [ ] **Step 4: On-target check.** Build + flash with `-DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_DISPLAY=1`. Expected: the radar view renders and its phantom count matches Ward's active churn set (cross-check the serial `churn` logs). Center-tap view cycling is added next task.

- [ ] **Step 5: 5-minute soak sanity.** Confirm BLE/Wi-Fi churn logs keep their normal cadence with the render task running (no radio starvation). If cadence degrades, lower fps / task priority.

- [ ] **Step 6: Commit.**
```bash
git add main/ward_display.c main/ward_display.h main/simulacra_main.c
git commit -m "feat(ward): render radar/status view on the C5 panel"
```

---

### Task 4: Bit-banged XPT2046 touch + calibration

**Files:**
- Modify: `main/ward_display.c` (touch init/read + view cycling in the loop)

**Interfaces:**
- Consumes: `main/ward_pins.h` touch pins.
- Produces: `static bool ward_touch_read(int *x, int *y)` used by the render loop for view navigation (Task 5 adds CONTROL taps).

- [ ] **Step 1: Port `touch_init` / `xpt_xfer` / `touch_read_xy`** from `cyd_main.c` (lines 354–411) into `ward_display.c`, renamed `ward_touch_*`, using the `WARD_PIN_T_*` macros. Keep the CYD calibration constants as **placeholders to be overwritten in Step 3**.

- [ ] **Step 2: Add a raw-coordinate debug log** temporarily: when `ward_touch_read` sees contact, `ESP_LOGW(TAG,"touch raw=(%d,%d)", rx, ry)` before applying calibration. Wire center-tap → `radar_ui_on_input` (cycle view) in the loop so navigation is testable.

- [ ] **Step 3: Calibrate on-hardware.** Flash, tap the four screen corners, record the raw `(rx,ry)` ranges from serial, and derive the axis mapping (the CYD's axes were **swapped and rawY inverted** — expect to rediscover this panel's own orientation). Replace the `WARD_TCAL_*` constants and the `px/py` mapping in `ward_touch_read` with the measured values. Remove the debug log.

- [ ] **Step 4: On-target check.** Tapping the screen center cycles views (RADAR → DETAIL → STATS → … → back). Confirm taps land where intended (a corner tap maps near that corner).

- [ ] **Step 5: Commit.**
```bash
git add main/ward_display.c
git commit -m "feat(ward): XPT2046 touch + panel calibration"
```

---

### Task 5: Local self-control view (no fleet reach)

**Files:**
- Modify: `main/ward_display.c` (CONTROL-view touch → local apply)
- Modify: `main/churn_selftest.c` (preset-mapping test)

**Interfaces:**
- Consumes: `sim_settings_apply_preset()`, `sim_settings_get_paused()` from `settings.h`; `radar_ctrl_select_next`, `radar_ctrl_mark_sent`, `ui.sel_preset` from `radar_ui.h`.
- Produces: local APPLY behavior; a `ST_CHECK` guarding `RADAR_CTRL_PRESET_COUNT == SIM_PRESET_COUNT`.

- [ ] **Step 1: Add the preset-alignment selftest** to `churn_selftest.c` (new `test_ward_control` called from the runner). This is the failing-first test — it fails to compile/link only if the counts diverge, so assert the invariant and the mapping:
```c
static void test_ward_control(void){
    ST_CHECK(RADAR_CTRL_PRESET_COUNT == SIM_PRESET_COUNT, "ctrl presets align with sim presets");
    sim_settings_t s;
    // sel_preset 3 (DENSE) applied locally resolves above the STEALTH floor on the C5 ceiling.
    ST_CHECK(sim_settings_apply_preset((sim_preset_t)3) == 0, "apply DENSE ok");
    sim_settings_get(&s);
    ST_CHECK(s.active_target >= SIM_TARGET_FLOOR, "DENSE respects floor");
    ST_CHECK(sim_settings_apply_preset((sim_preset_t)SIM_PRESET_PAUSE) == 0, "apply PAUSE ok");
    sim_settings_get(&s);
    ST_CHECK(s.paused, "PAUSE freezes rotation");
}
```

- [ ] **Step 2: Run the selftest, expect PASS.** `idf.py -B build_c5 -DCHURN_SELFTEST=1 -DSIMULACRA_DISPLAY=1 build` then flash; serial shows `selftest: N/N pass` including the new checks. (If the count assert fails, reconcile `RADAR_CTRL_PRESET_COUNT` with `SIM_PRESET_COUNT` before proceeding.)

- [ ] **Step 3: Wire the CONTROL-view taps** in the render loop. Copy the CYD's CONTROL zone logic (`cyd_main.c` 458–470) but replace the SEND action with a **local** apply — no signing, no ESP-NOW:
```c
if (ui.view == RADAR_VIEW_CONTROL) {
    radar_ui_note_input(&ui, now);
    if (ty > 200 && tx > 60 && tx < 180) {          // APPLY button (local)
        sim_settings_apply_preset((sim_preset_t)ui.sel_preset);   // clamps to C5 ceiling, persists NVS
        radar_ctrl_mark_sent(&ui, now);             // reuse the SENT flash as "APPLIED"
        ESP_LOGW(TAG, "ward: applied local preset %u", ui.sel_preset);
    } else if (tx < 80) {
        for (int i=0;i<RADAR_CTRL_PRESET_COUNT-1;i++) radar_ctrl_select_next(&ui);  // prev
    } else if (tx > 160) {
        radar_ctrl_select_next(&ui);                // next
    } else {
        radar_ui_on_input(&ui, now);                // center = leave view
    }
}
```
Seed `ui.sel_preset` from the current settings when entering CONTROL so it reflects reality (including presets Vigil pushed). **Do not** add `#include "sim_ctrl_sk.h"` or any signing — Ward has no key.

- [ ] **Step 4: On-target check.** Enter CONTROL, pick DENSE, tap APPLY → Ward's phantom count rises to its C5 ceiling and the SENT/APPLIED flash shows; reboot → the preset persists (NVS). Pick PAUSE + APPLY → rotation freezes.

- [ ] **Step 5: No-fleet-reach check.** With a second decoy on-air, Ward's local APPLY must **not** change the second decoy's phantom count (proves there is no send path). Confirm from the second board's serial.

- [ ] **Step 6: Commit.**
```bash
git add main/ward_display.c main/churn_selftest.c
git commit -m "feat(ward): local self-control view (no fleet reach)"
```

---

### Task 6: SD librarian — persist the learned DB

**Files:**
- Create: `main/ward_librarian.c`, `main/ward_librarian.h`
- Modify: `main/CMakeLists.txt` (SRCS), `main/simulacra_main.c` (init + tick), `main/churn_selftest.c` (round-trip test), `sdkconfig.defaults` (FATFS LFN)

**Interfaces:**
- Consumes: `learn_snapshot()`, `learn_ingest_wire()` (`learn.h`); `learn_db_seal/open`, `learn_db_derive_key` (`learn_db.h`); `SIMULACRA_ESPNOW_KEY` (`radar_key.h`).
- Produces: `void ward_librarian_init(void)` (mount SD, derive key, restore at boot), `void ward_librarian_tick(void)` (throttled dirty save). Consumed by `simulacra_main.c`.

- [ ] **Step 1: Add the round-trip selftest** to `churn_selftest.c` (`test_ward_librarian`) — buffer-level, no filesystem, so it runs on any build:
```c
static void test_ward_librarian(void){
    uint8_t key[32]; learn_db_derive_key(SIMULACRA_ESPNOW_KEY, key);
    learned_template_t recs[3]; memset(recs,0,sizeof recs);
    for (int i=0;i<3;i++){ recs[i].company_id = 0x1000+i; recs[i].sightings = i+1; }
    static uint8_t blob[sizeof(learn_db_hdr_t)+3*sizeof(learned_template_t)]; size_t blen=0;
    ST_CHECK(learn_db_seal(blob,&blen,recs,3,key)==0, "librarian seal ok");
    learned_template_t out[3]; uint16_t cnt=0;
    ST_CHECK(learn_db_open(blob,blen,out,&cnt,key)==0 && cnt==3, "librarian open round-trips");
    blob[blen-1] ^= 0xFF;   // corrupt the GCM tag
    ST_CHECK(learn_db_open(blob,blen,out,&cnt,key)!=0, "librarian rejects tampered db");
}
```

- [ ] **Step 2: Run selftest, expect PASS** (`-DCHURN_SELFTEST=1` build, flash, read `selftest: N/N pass`).

- [ ] **Step 3: Write `main/ward_librarian.{c,h}`** porting the CYD's SD path (`cyd_main.c` `sd_mount`, `learn_db_load`, `learn_db_save`) but sourcing records from the decoy's own store via `learn_snapshot()` and restoring via `learn_ingest_wire()`:
```c
// ward_librarian.c (sketch of the two hot paths)
void ward_librarian_init(void){
    learn_db_derive_key(SIMULACRA_ESPNOW_KEY, s_key);
    s_sd_ok = ward_sd_mount();                       // SPI2 shared: host already inited by display
    if (!s_sd_ok) { ESP_LOGW(TAG,"sd absent -> RAM-only"); return; }
    mkdir("/sdcard/simulacra", 0777);
    FILE *f = fopen(LEARN_DB_PATH,"rb"); if(!f){ ESP_LOGW(TAG,"no learndb (fresh)"); return; }
    /* read, learn_db_open(...), then per record learn_ingest_wire(&rec); log count */
}
void ward_librarian_tick(void){                       // called periodically from the render loop
    if (!s_sd_ok) return;
    uint32_t now = (uint32_t)(esp_timer_get_time()/1000);
    if (now - s_last_save_ms < LEARN_DB_SAVE_MS) return;
    static learned_template_t snap[LEARN_CAP]; size_t n = learn_snapshot(snap, LEARN_CAP);
    if (n==0) return;
    /* learn_db_seal(snap,n) -> write LEARN_DB_TMP -> rename() to LEARN_DB_PATH (atomic) */
    s_last_save_ms = now;
}
```
**SD-on-SPI2 note:** the display already called `spi_bus_initialize(WARD_SPI_HOST,...)`; `ward_sd_mount` must NOT re-init the bus — configure `sdspi_device_config_t.host_id = WARD_SPI_HOST`, `gpio_cs = WARD_PIN_SD_CS`, and mount with `esp_vfs_fat_sdspi_mount`. Gate `ward_librarian_init` to run **after** `ward_display_hw_init`. **Physical wiring:** this FEIYANG panel exposes SD as a separate 4-pin header — jumper `SD_SCK→`GPIO6, `SD_MOSI→`GPIO23, `SD_MISO→`GPIO24 onto the shared bus; only `SD_CS`→GPIO9 is dedicated. If the SD proves flaky sharing the 40 MHz display clock, drop the SD transfer speed in the mount config (sdspi negotiates its own clock, so this is usually a non-issue).

- [ ] **Step 4: FATFS long-filename support.** Add to `sdkconfig.defaults`:
```
CONFIG_FATFS_LFN_STACK=y
CONFIG_FATFS_MAX_LFN=255
```
(delete the live `build_c5/sdkconfig` or re-run set-target so the defaults seed a fresh config — `.defaults` only seeds a NEW config).

- [ ] **Step 5: Wire init + tick.** Add `ward_librarian.c` to SRCS; in `simulacra_main.c` (behind `#if SIMULACRA_DISPLAY`) call `ward_librarian_init()` after `ward_display_start()`, and call `ward_librarian_tick()` from the render loop each iteration.

- [ ] **Step 6: On-target check.** With learned templates present, confirm serial `sd: mounted`, a periodic save, and that `/sdcard/simulacra/learn.db` grows. Hard-reset Ward → boot log shows N records ingested from the card. Pull the card → boots RAM-only, no crash.

- [ ] **Step 7: Commit.**
```bash
git add main/ward_librarian.c main/ward_librarian.h main/CMakeLists.txt main/simulacra_main.c main/churn_selftest.c sdkconfig.defaults
git commit -m "feat(ward): SD librarian — encrypted-at-rest learned DB persistence"
```

---

### Task 7: Full-system bring-up + verification soak

**Files:**
- Modify: `docs/hardware/` (add a short `ward-display-build.md` wiring + flash note)

**Interfaces:** none (integration task).

- [ ] **Step 1: Clean Ward build** with the full flag set and run the on-target checklist from the spec (§Testing, checks 1–6): display up, touch calibrated, local apply (incl. persistence), no fleet reach, Vigil still commands Ward (signed CONFIG still applies + CONTROL view reflects it), SD librarian round-trip.

- [ ] **Step 2: Run the selftest build once more** (`-DCHURN_SELFTEST=1`) and confirm `selftest: N/N pass` with the two new Ward tests included.

- [ ] **Step 3: 10-minute soak** with Ward + a second decoy + Vigil: confirm churn cadence is unaffected by rendering, learn sync + SD saves proceed, and no reboots/stack warnings. (TweetNaCl is unused on Ward, so the default main stack should suffice — confirm no overflow warning; only bump `CONFIG_ESP_MAIN_TASK_STACK_SIZE` if the soak shows a problem.)

- [ ] **Step 4: Write `docs/hardware/ward-display-build.md`** — the verified pin map, the flash recipe, and any bring-up quirks discovered (color/mirror flags, touch calibration values). Commit.
```bash
git add docs/hardware/ward-display-build.md
git commit -m "docs(hardware): Ward display wiring + bring-up notes"
```

---

## Self-Review

**Spec coverage:** Every spec scope item maps to a task — build gate + pins (T1), display HAL + render (T2/T3), touch (T4), local self-control (T5), SD librarian (T6), full verification (T7). Security posture (no key, no send path) is enforced in T5 Step 3 and checked in T5 Step 5. Out-of-scope items (fleet control, Shade haptics, second panel) appear in no task. ✓

**Placeholder scan:** The pin GPIO numbers are explicitly provisional with a mandatory Task-1 schematic-verification step and a single-header change surface — this is a hardware unknown, not a plan gap. `ward_librarian.c` Step 3 is a sketch of the two hot paths with the exact APIs named; the file-IO boilerplate (fread/fwrite/rename) is standard and the atomic-write pattern is specified. All test code is concrete. ✓

**Type consistency:** `sel_preset` (0..4) ↔ `sim_preset_t` alignment is asserted (T5 S1). `radar_flush_fn` signature matches `ward_flush`. `learn_snapshot`/`learn_ingest_wire`/`learn_db_seal`/`learn_db_open`/`learn_db_derive_key` signatures match `learn.h`/`learn_db.h`. `sim_settings_apply_preset`/`sim_settings_get`/`sim_settings_get_paused` match `settings.h`. ✓
