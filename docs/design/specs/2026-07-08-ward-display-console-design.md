# Ward Display Console — Design Spec

**Date:** 2026-07-08
**Status:** Draft (brainstorming → spec). Supersedes nothing; net-new feature.
**Persona affected:** Ward (ESP32-C5, fixed / vehicle decoy). Shade (C6) and Vigil (CYD) unchanged.

## Goal

Give **Ward** (the fixed/vehicle C5 decoy) its own 2.4″ SPI screen so it becomes a
self-contained node: it renders its own status + the fleet radar, lets the operator
adjust **its own** churn density by touch, and persists the fleet's learned-template DB
to a local microSD as a second durable copy. Ward gains **no authority over other
decoys** — the asymmetric fleet-control guarantee from the Vigil feature is preserved.

## Context & motivation

The operator has two bare **SBT240-W61 V01** panels: 2.4″ 240×320 SPI TFT, ILI9341
display + XPT2046 resistive touch + a microSD slot, **no onboard MCU** (identical
controller family to the CYD "Cheap Yellow Display" that Vigil already runs on). One
panel is assigned to Ward; the second is a bench/spare.

**Ward host board:** confirmed **ESP32-C5-WIFI6-KIT-N16R8** (ESP32-C5-WROOM-1 module,
dual USB-C, 16 MB flash / 8 MB PSRAM) — *not* the DevKitC-1. This matters for wiring: the
N16R8 module breaks out only GPIO `{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,23,24,25,26,27,28}`
on the two header blocks; **GPIO16–22 are not exposed** (internal to the module's
flash/PSRAM). The board has no pre-soldered headers.

Design decisions already settled during brainstorming:

- **Distributed, not one-box.** Ward becomes its own screened unit; Vigil stays the
  controller; Shade (C6, EDC) stays screenless and covert (haptic alerting is a
  separate future feature). See `[[feature-backlog]]`.
- **Ward's authority = monitor + local self-control + SD librarian.** Explicitly **not**
  a fleet controller. Rationale: Ward is physically deployed in a vehicle and is more
  capturable than a CYD kept on the operator; putting the Ed25519 **private** control
  key on Ward would weaken the "capturing a decoy ≠ fleet control" property that the
  Vigil feature (`docs/design/specs/2026-07-07-vigil-runtime-settings-control-design.md`)
  was built to guarantee. Ward therefore holds **no private key** and can only change
  **its own** settings.

## Scope

**In scope**
1. C5 display HAL (ILI9341 over `esp_lcd`) + render of the existing radar/status views.
2. Bit-banged XPT2046 touch with a per-panel calibration (ported from the CYD).
3. A **local self-control** view: touch cycles a preset and applies it to **Ward's own**
   churn engine via `sim_settings_apply_preset()` — no ESP-NOW send, no signing.
4. A **SD librarian**: mount the panel's microSD and persist Ward's in-RAM learned
   template set to `/sdcard/simulacra/learn.db` (AES-256-GCM), restoring it at boot.
5. A build gate (`SIMULACRA_DISPLAY`) so the screen/SD/touch code compiles only for the
   Ward build; the normal headless C5 and the C6/Shade builds are unaffected.
6. A centralized, macro-driven pin map with a mandatory bring-up verification step.

**Out of scope (explicitly)**
- Fleet control from Ward (no private key, no `RADAR_TYPE_CONFIG` **send** path).
- Exposing the **detection** toggle over any remote channel (web-UI-only, unchanged).
- Shade haptic alerting (separate feature).
- The second SBT240 panel (bench/spare; no firmware target this round).
- Any change to Vigil (CYD) or to the signed-CONFIG fleet-control path.

## Architecture

Ward is a decoy (`main/` firmware). Today it runs headless: radios + churn + learn +
ESP-NOW responder. This feature adds an optional **presentation + local-control + SD
persistence layer** behind `#if SIMULACRA_DISPLAY`, reusing code that already exists:

| Capability            | Reused from                                   | New on C5 |
|-----------------------|-----------------------------------------------|-----------|
| Radar/status render   | `components/simulacra_radar` (`radar_render`) | display HAL only |
| ILI9341 driver        | `esp_lcd_ili9341` managed component           | SPI2 bus init |
| Touch (XPT2046)       | CYD `touch_read_xy` / `xpt_xfer` (bit-bang)   | pins + fresh cal |
| Local settings apply  | `main/settings.{h,c}` (`sim_settings_*`)      | touch→apply wiring |
| Learned-DB seal/open  | `learn_db_seal/open`, `learn_db_derive_key`   | SD file IO on Ward |
| SD mount (sdspi)      | CYD `sd_mount()` pattern                       | SPI2 shared bus |

### Hardware wiring (SBT240-W61 ↔ ESP32-C5-WIFI6-KIT-N16R8)

The board ships with **no pre-soldered headers** — the operator solders to broken-out
through-holes. Of the 22 exposed GPIOs, these are **off-limits**: strapping pins **GPIO2,
7, 25, 26, 27, 28** (GPIO27 also drives the RGB LED); UART0 console **GPIO11/12**; native
USB **GPIO13/14**. That leaves **12 usable pins** `{0,1,3,4,5,6,8,9,10,15,23,24}`.

**Pin budget is tight.** The full wiring needs 13 signals but only 12 pins are free, so the
**backlight is tied to 3V3** (always on — Ward is powered, not battery), dropping it to 12
signals that fit exactly. Display **RST** uses GPIO3 (an output-only JTAG/MTDI pad, safe
post-boot). The verified map lives entirely in `main/ward_pins.h`.

**Bus plan (cleaner than the CYD):** the C5's general-purpose **SPI2 (GPSPI2)** is free
(the WROOM module's flash/PSRAM SPI is internal, not on the header). Display and SD share
**SPI2** as two devices with separate CS lines (IDF arbitrates the shared bus). Touch stays
on the **proven bit-banged XPT2046** path from the CYD (its own 5 GPIOs), sidestepping any
display/SD ↔ touch clock conflict and maximizing code reuse.

**Confirmed pin map (from the board pinout; verify GPIO0/1 have no 32 kHz crystal):**

| Signal            | Net on SBT240        | C5 GPIO | Notes |
|-------------------|----------------------|---------|-------|
| SPI2 SCLK         | `SCK` + `SD_SCK`     | 6       | shared display+SD clock |
| SPI2 MOSI         | `SDI` + `SD_MOSI`    | 23      | shared |
| SPI2 MISO         | `SD_MISO`            | 24      | SD read; display MISO unused |
| Display CS        | `CS`                 | 15      | |
| Display DC/RS     | `DC`                 | 8       | |
| Display RST       | `RST`                | 3       | MTDI/JTAG pad, output-only |
| SD CS             | `SD_CS`              | 9       | |
| Touch CLK         | `T_CLK`              | 0       | out (verify no 32 kHz xtal) |
| Touch CS          | `T_CS`               | 1       | out (verify no 32 kHz xtal) |
| Touch DIN         | `T_DIN`              | 4       | out (MTCK/JTAG) |
| Touch DO          | `T_DO`               | 5       | in  (MTDO/JTAG) |
| Touch IRQ         | `T_IRQ`              | 10      | in, idle-high |
| Backlight         | `LED` / `BL`         | —       | **tie to 3V3** (always on) |
| Power             | `VCC` (3V3), `GND`   | 3V3/GND | drive panel VCC from 3V3 |

> Caveats to confirm at bring-up: GPIO0/1 double as the 32.768 kHz crystal pads — usable as
> assigned **unless a 32 kHz crystal is populated** (none visible in the board photos);
> GPIO3/4/5 are JTAG (no HW debugger while in use). Want backlight dimming later? Reclaim
> GPIO13 (native USB) since programming/monitoring goes over the separate UART-USB port.

### Firmware components

1. **Build gate `SIMULACRA_DISPLAY`.** Added to the `foreach` `-D` forwarder in
   `main/CMakeLists.txt` (same mechanism as `SIMULACRA_ESPNOW`, `SIMULACRA_CONFIG_CTRL`).
   Default 0. All new files/init are wrapped `#if SIMULACRA_DISPLAY`. The Ward build flashes
   `-DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_DISPLAY=1`.

2. **`main/ward_pins.h`** — every display/touch/SD GPIO + the SPI2 host id as `#define`s.
   Single source of truth for the wiring; the only file that changes if the pin map moves.

3. **`main/ward_display.{c,h}`** — C5 presentation layer:
   - `ward_display_init()` — init SPI2, `esp_lcd_panel_io_spi` + `esp_lcd_ili9341`, backlight,
     touch GPIOs, SD mount. Returns which subsystems came up (display always required;
     SD optional → RAM-only librarian if absent, mirroring the CYD's graceful degrade).
   - A render task that draws the current view via `radar_render` primitives at a modest
     rate (e.g. 5–10 fps) and services touch.
   - Views: **STATUS** (Ward's own phantom count / preset / follower + tracker alerts,
     reusing the radar view), and **CONTROL** (local self-control, below). Center-tap
     cycles views, mirroring the Vigil UX vocabulary.

4. **Local self-control (CONTROL view).** `◀ [PRESET] ▶` + an **APPLY** button. On APPLY,
   Ward calls `sim_settings_apply_preset(sel)` **directly** — this clamps to Ward's own
   `CHURN_ACTIVE_SET` ceiling (C5 = 8), applies to the churn engine, and persists to NVS.
   **No ESP-NOW frame is sent and no signature is produced** (the whole point: Ward has no
   private key and no fleet reach). The view seeds `sel` from `sim_settings_get()` /
   `sim_settings_get_paused()` so it reflects reality (including presets pushed by Vigil).
   Precedence is **last-writer-wins**: a subsequent signed CONFIG from Vigil, or a
   subsequent local APPLY, simply overwrites the shared settings backend + NVS. This is
   acceptable and documented; no locking.

5. **SD librarian (`main/ward_librarian.{c,h}`, gated with the display).**
   - **Key:** `learn_db_derive_key(SIMULACRA_ESPNOW_KEY, key)` (HKDF-SHA256), identical to
     Vigil. Ward already holds the ESP-NOW PSK.
   - **Save:** periodically (and on meaningful change) `learn_snapshot()` → `learn_db_seal()`
     → **atomic** write to `/sdcard/simulacra/learn.db` (temp file + rename), throttled
     (e.g. ≥60 s between writes) to spare the card.
   - **Load at boot:** read the file → `learn_db_open()` (authenticates the GCM tag) → for
     each record `learn_ingest_wire()` (re-gates + max-merges into the live store). A bad
     tag / missing file → skip, RAM-only, log a warning. This warm-starts Ward with the
     durable library and lets it keep contributing under ESP-NOW RAM sync.
   - **FATFS gotcha (from Vigil bring-up):** long filenames need `CONFIG_FATFS_LFN_STACK`
     + `CONFIG_FATFS_MAX_LFN=255` in the C5 `sdkconfig.defaults`.

### Security posture

- Ward stores **no Ed25519 private key**. `cyd/main/sim_ctrl_sk.h` stays CYD-only.
- Ward's only write authority is `sim_settings_apply_preset()` on **itself**, which is
  already floor-clamped (`SIM_TARGET_FLOOR = 4`, STEALTH density) so even a misbehaving
  local UI can't thin Ward's own cover below the safe floor.
- The SD `learn.db` is AES-256-GCM sealed with an HKDF-derived key; a pulled card yields
  only ciphertext (learned advert **shapes**, never identities — unchanged from Vigil).
- No new remote-reachable surface: Ward still only **listens** for `RADAR_TYPE_CONFIG`
  (existing, signed) and never gains a send path. The detection toggle remains web-UI-only.

## Data flow

```
                 ESP-NOW (existing)                     touch (new)
Vigil ── signed CONFIG ─▶ Ward decoy ◀── learn RAM-sync ──▶ other decoys
                              │  ▲                              APPLY │
                              ▼  │ learn_ingest_wire (boot)           ▼
                     sim_settings_apply_preset          sim_settings_apply_preset
                              │                          (LOCAL, no key, no send)
                              ▼
                         churn engine ── learn_snapshot ──▶ learn_db_seal ──▶ /sdcard/…/learn.db
                                                        ◀── learn_db_open ◀── (boot restore)
                              │
                              ▼
                     radar_render ──▶ ILI9341 (SPI2)   ◀── XPT2046 touch (bit-bang)
```

## Testing strategy

**Host/selftest (no hardware, `-DCHURN_SELFTEST=1` style):**
- CONTROL view state machine: preset cycling wraps correctly; APPLY calls
  `sim_settings_apply_preset` with the selected preset; seeding from `sim_settings_get`.
- Librarian round-trip: `learn_snapshot` → `learn_db_seal` → `learn_db_open` →
  `learn_ingest_wire` reproduces the store; tampered ciphertext / wrong key is rejected
  (reuses the existing `learn_db` tamper tests).

**On-target (Ward C5 + one other decoy + Vigil):**
1. **Display up:** ILI9341 renders the radar/status view; backlight controllable.
2. **Touch calibrated:** 4-corner tap maps to screen coords; APPLY button reachable
   (expect the same swapped/inverted-axis discovery the CYD needed — budget a cal pass).
3. **Local apply:** cycling to DENSE + APPLY raises Ward's own phantom count to its C5
   ceiling; PAUSE freezes rotation; NVS persists across reboot.
4. **No fleet reach:** local APPLY changes **only** Ward — the second decoy's phantom
   count is unaffected (proves no send path).
5. **Vigil still commands Ward:** a signed CONFIG from Vigil still applies on Ward and the
   CONTROL view reflects the new preset (proves last-writer-wins + no regression).
6. **SD librarian:** learned templates persist to `/sdcard/simulacra/learn.db`; hard reset
   → Ward boots and restores the library (log shows N records ingested); absent card →
   RAM-only, no crash.

## Open questions / risks

1. **Pin map (resolved to the real board).** Now mapped to the exposed GPIOs of the
   ESP32-C5-WIFI6-KIT-N16R8 (see table). Remaining bench checks: GPIO0/1 free of a 32 kHz
   crystal, and the JTAG pads (3/4/5) not needed for HW debug. All in one header.
2. **Panel VCC.** Confirm the SBT240 runs at 3.3 V logic + power (expected for this family)
   before wiring; some panels want 5 V for the backlight. Since the backlight is tied to a
   rail (not a GPIO), pick 3V3 vs 5V based on the panel's LED spec at bring-up.
3. **Render task vs. radio timing.** The decoy's radios are latency-sensitive; the render
   task must run at low priority / modest fps so it never starves BLE/Wi-Fi churn. Mitigate
   with a capped fps and a dedicated low-priority task; validate in the 10-min soak.
4. **Stack.** TweetNaCl is **not** used on Ward (no signing), so the CYD's 12288-byte
   main-stack bump is unnecessary — but confirm the render + SD paths fit the default
   task stacks; bump only if a soak shows headroom issues.
5. **Librarian semantics.** v1 persists Ward's **own RAM-synced** learned set (via
   `learn_snapshot`), which under ESP-NOW RAM sync already converges toward the fleet
   library. Full Vigil-style `VIGIL_LIB_CAP` aggregation is **not** duplicated onto Ward
   this round; if a divergence is observed in soak, revisit.

## Task decomposition (preview — detailed steps go in the plan)

1. `ward_pins.h` + schematic-verified pin map + `SIMULACRA_DISPLAY` build gate.
2. C5 display HAL bring-up (`ward_display.c`: SPI2 + ILI9341 + backlight) — render a test frame.
3. Port radar/status render onto the C5 display.
4. Bit-banged XPT2046 touch + 4-corner calibration.
5. CONTROL view + local `sim_settings_apply_preset` wiring + selftest.
6. SD mount (SPI2 shared) + `ward_librarian` seal/open persistence + boot restore + selftest.
7. C5 `sdkconfig.defaults` (FATFS LFN) + CMake wiring + Ward build recipe.
8. On-target bring-up + the six hardware checks above + a 10-min soak.

---

_Related: `[[vigil-remote-control]]` (settings backend + signed CONFIG this reuses),
`[[self-learning-templates]]` (learn DB + AES-GCM persistence this ports),
`[[espnow-remote-display]]` (radar render + CYD display/touch this ports),
`[[modular-multiboard-roles]]` (persona split rationale)._
