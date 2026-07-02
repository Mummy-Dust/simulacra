# Remote ESP-NOW display (CYD) — implementation plan

**Goal:** A physically-decoupled "threat radar" screen: the C5/C6 decoy answers on-demand, AES-GCM-encrypted telemetry requests over ESP-NOW; a Cheap Yellow Display (classic ESP32) renders the radar.

**Architecture:** A shared, mostly-pure component `components/simulacra_radar/` (radar geometry, view/state machine, gfx primitives + renderers, and the AES-GCM wire layer) is compiled by both firmwares. The decoy gains a listen-only responder `main/esp_now_link.{h,c}` (gated `SIMULACRA_ESPNOW`); a separate ESP-IDF app `cyd/` (target `esp32`) drives the CYD panel + touch and receives/renders. The whole snapshot fits one ESP-NOW frame (~151 B).

**Tech Stack:** ESP-IDF, ESP-NOW, mbedTLS AES-GCM, `esp_lcd` (ST7789/ILI9341 + XPT2046 touch managed components), C.

## Global Constraints

- **Shared component auto-discovery:** ESP-IDF auto-includes a top-level `components/` dir, so `components/simulacra_radar/` is picked up by the decoy build automatically; the `cyd/` app references it via `EXTRA_COMPONENT_DIRS`. The decoy's `main/CMakeLists.txt` must add `simulacra_radar` to `REQUIRES` to call it from `churn_selftest.c`.
- **Build-flag gate is a source `#define`:** `SIMULACRA_ESPNOW` in `main/simulacra_main.c`, default **0**. Independent of `SIMULACRA_DISPLAY`.
- **Pure vs hardware:** geometry, view/FSM, the wire layer (pack/unpack + AES-GCM seal/open + replay), and the `webui_status_t → radar_wire_status_t` converter are pure and self-tested in `main/churn_selftest.c` (`ST_CHECK`, run with `#define CHURN_SELFTEST 1`, expect `SELFTEST result: PASS (fails=0)`, revert to 0 to ship). Radio/panel verified on-target via build-flash-read.
- **Wire format:** `[magic(2)=0x5A,0x4D | ver(1)=1 | type(1) | nonce(12) | ciphertext | tag(16)]`; `magic|ver|type` authenticated as GCM AAD; nonce = `salt(4) | counter(8, big-endian)`; STATUS payload = packed `radar_wire_status_t`, REQUEST payload = 4-byte client nonce. Frame ≤ 250 B.
- **Crypto:** mbedTLS AES-256-GCM under a 32-byte pre-shared `SIMULACRA_ESPNOW_KEY` (placeholder default in `components/simulacra_radar/radar_key.h`, documented "CHANGE ME", never a real key committed). Bidirectional replay rejection on `(salt, counter)`.
- **Channel & opsec:** fixed channel **1** (`SIMULACRA_ESPNOW_CHANNEL`); decoy broadcast-addressed, random locally-administered source MAC per boot, silent until a valid REQUEST, then 3 back-to-back STATUS frames in the current dwell. Law 3: hash-only threats, aggregates only.
- **Targets / ports / IDF:** CYD = `esp32`, COM14, IDF 5.4. C5 (Ward) = COM12, IDF 5.5. SparkFun C6 (Shade) = COM13, IDF 5.4. Force Python 3.12.
- **Both chips little-endian** (RISC-V + Xtensa) → packed struct crosses the wire without byte-swapping.
- **Commits:** `feat(espnow):` / `docs(espnow):`, author Mummy-Dust, **no AI-tooling references, no `Co-Authored-By`. Commit locally, do NOT push.**

## File Structure

- `components/simulacra_radar/CMakeLists.txt` (new) — component registration (`REQUIRES mbedtls`).
- `components/simulacra_radar/radar_geom.{h,c}` (new) — pure radar geometry.
- `components/simulacra_radar/radar_ui.{h,c}` (new) — pure view/backlight state machine.
- `components/simulacra_radar/radar_wire.{h,c}` (new) — `radar_wire_status_t`, framing, AES-GCM seal/open, replay.
- `components/simulacra_radar/radar_key.h` (new) — the pre-shared key placeholder.
- `components/simulacra_radar/radar_gfx.{h,c}` + `radar_font.h` (new, Task 7) — band-buffer primitives + font.
- `components/simulacra_radar/radar_render.{h,c}` (new, Task 7) — the three view renderers over a flush callback.
- `main/esp_now_link.{h,c}` (new, Task 5) — decoy responder + the `webui`→wire converter.
- `main/CMakeLists.txt` (modify) — add `simulacra_radar` to `REQUIRES`; add `esp_now_link.c` to `SRCS`.
- `main/simulacra_main.c` (modify) — `SIMULACRA_ESPNOW` gate + `esp_now_link_start()`.
- `main/churn_selftest.c` (modify) — tests for geometry, FSM, wire, converter.
- `cyd/` (new, Task 6+) — the CYD ESP-IDF app.

---

### Task 1: Shared component scaffold + radar geometry (pure, self-tested)

**Files:**
- Create: `components/simulacra_radar/CMakeLists.txt`, `components/simulacra_radar/radar_geom.h`, `components/simulacra_radar/radar_geom.c`
- Modify: `main/CMakeLists.txt` (add `simulacra_radar` to `REQUIRES`)
- Modify: `main/churn_selftest.c` (`#include "radar_geom.h"`, `test_radar_geometry()`, register it)

**Interfaces:**
- Produces: `uint16_t radar_rssi_to_radius(int8_t rssi, uint16_t r_min, uint16_t r_max)`; `uint16_t radar_hash_to_angle(uint32_t hash)`; `void radar_polar_to_xy(int cx, int cy, uint16_t r, uint16_t angle_deg, int *x, int *y)`.

- [ ] **Step 1: Write `components/simulacra_radar/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "radar_geom.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls
)
```

- [ ] **Step 2: Write `components/simulacra_radar/radar_geom.h`**

```c
#pragma once
#include <stdint.h>

// Map a follower's best RSSI (dBm, negative) to a radar radius in px. Stronger (less
// negative) -> smaller radius (nearer center). Banded: >=-45 near, -45..-70 mid, <-70 far.
// Result within [r_min, r_max].
uint16_t radar_rssi_to_radius(int8_t rssi, uint16_t r_min, uint16_t r_max);

// Stable pseudo-bearing from a hash: [0,359] deg. Same hash -> same angle (synthetic; a
// single antenna cannot triangulate — labeled as such on-screen).
uint16_t radar_hash_to_angle(uint32_t hash);

// Polar (center cx,cy; radius r; angle deg, 0=east, CCW) -> screen x,y (y grows down).
void radar_polar_to_xy(int cx, int cy, uint16_t r, uint16_t angle_deg, int *x, int *y);
```

- [ ] **Step 3: Write the failing test in `main/churn_selftest.c`**

Add `#include "radar_geom.h"` to the includes, then add above `churn_selftest_run`:

```c
static void test_radar_geometry(void)
{
    uint16_t near = radar_rssi_to_radius(-40, 10, 100);
    uint16_t mid  = radar_rssi_to_radius(-60, 10, 100);
    uint16_t far  = radar_rssi_to_radius(-80, 10, 100);
    ST_CHECK(near < mid && mid < far, "radar radius grows with distance");
    ST_CHECK(near >= 10 && far <= 100, "radar radius within clamp");
    ST_CHECK(radar_rssi_to_radius(-20, 10, 100) == near, "very strong clamps near");
    ST_CHECK(radar_rssi_to_radius(-99, 10, 100) == far,  "very weak clamps far");
    ST_CHECK(radar_hash_to_angle(0xABCD1234) == radar_hash_to_angle(0xABCD1234),
             "angle stable per hash");
    ST_CHECK(radar_hash_to_angle(0xFFFFFFFF) < 360, "angle in range");
    int x, y;
    radar_polar_to_xy(120, 160, 50, 0, &x, &y);
    ST_CHECK(x == 170 && y == 160, "0deg due east");
    radar_polar_to_xy(120, 160, 50, 90, &x, &y);
    ST_CHECK(x >= 119 && x <= 121 && y >= 109 && y <= 111, "90deg due north");
}
```

Register `test_radar_geometry();` in `churn_selftest_run` after `test_display_ui();` if present, else after `test_webui_json();`. Add `simulacra_radar` to `main/CMakeLists.txt` `REQUIRES`:
```cmake
    REQUIRES bt nvs_flash driver esp_wifi esp_netif esp_event esp_http_server simulacra_radar
```

- [ ] **Step 4: Run to verify it fails**

Set `#define CHURN_SELFTEST 1` in `main/simulacra_main.c`, then (C5, IDF 5.5, Python 3.12):
```
idf.py build
```
Expected: **link error** `undefined reference to radar_rssi_to_radius`.

- [ ] **Step 5: Write `components/simulacra_radar/radar_geom.c`**

```c
#include "radar_geom.h"
#include <math.h>

uint16_t radar_rssi_to_radius(int8_t rssi, uint16_t r_min, uint16_t r_max)
{
    uint16_t band;                                   // 0 near, 1 mid, 2 far
    if      (rssi >= -45) band = 0;
    else if (rssi >= -70) band = 1;
    else                  band = 2;
    return (uint16_t)(r_min + (uint32_t)(r_max - r_min) * band / 2u);
}

uint16_t radar_hash_to_angle(uint32_t hash) { return (uint16_t)(hash % 360u); }

void radar_polar_to_xy(int cx, int cy, uint16_t r, uint16_t angle_deg, int *x, int *y)
{
    double a = (double)angle_deg * (M_PI / 180.0);
    *x = cx + (int)lround((double)r * cos(a));
    *y = cy - (int)lround((double)r * sin(a));
}
```

- [ ] **Step 6: Run to verify it passes**

```
idf.py build && idf.py -p COM12 flash
```
Read serial (build-flash-read, `-Grep 'SELFTEST'`). Expected: `PASS (fails=0)`, count raised.

- [ ] **Step 7: Commit**

```
git add components/simulacra_radar/CMakeLists.txt components/simulacra_radar/radar_geom.h components/simulacra_radar/radar_geom.c main/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(espnow): shared simulacra_radar component + radar geometry (self-tested)"
```
Set `CHURN_SELFTEST 0` back.

---

### Task 2: View + backlight state machine (pure, self-tested)

**Files:**
- Create: `components/simulacra_radar/radar_ui.h`, `components/simulacra_radar/radar_ui.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `radar_ui.c`)
- Modify: `main/churn_selftest.c` (`test_radar_ui()`, register)

**Interfaces:**
- Produces: `radar_view_t` enum; `radar_ui_t`; `void radar_ui_reset(radar_ui_t*, uint32_t now_ms, uint8_t threat_count)`; `void radar_ui_on_input(radar_ui_t*, uint32_t now_ms)`; `void radar_ui_on_tick(radar_ui_t*, uint32_t now_ms, uint8_t threat_count)`.

- [ ] **Step 1: Write `components/simulacra_radar/radar_ui.h`**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RADAR_VIEW_IDLE_MS 15000u   // no input this long -> back to radar
#define RADAR_BL_IDLE_MS   30000u   // clear + idle this long -> backlight off

typedef enum { RADAR_VIEW_RADAR = 0, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS, RADAR_VIEW_COUNT } radar_view_t;

typedef struct {
    radar_view_t view;
    uint32_t last_input_ms;
    uint32_t last_wake_ms;
    bool     backlight_on;
    uint8_t  last_threat_count;
} radar_ui_t;

void radar_ui_reset(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count);
// A user input (button press or screen touch): advance view, wake, reset timers.
void radar_ui_on_input(radar_ui_t *ui, uint32_t now_ms);
// Periodic: idle-return to radar; backlight off when clear+idle; wake+radar on a new follower.
void radar_ui_on_tick(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count);
```

- [ ] **Step 2: Write the failing test in `main/churn_selftest.c`**

Add `#include "radar_ui.h"`, then above `churn_selftest_run`:

```c
static void test_radar_ui(void)
{
    radar_ui_t ui;
    radar_ui_reset(&ui, 1000, 0);
    ST_CHECK(ui.view == RADAR_VIEW_RADAR && ui.backlight_on, "reset: radar + bl on");
    radar_ui_on_input(&ui, 1100); ST_CHECK(ui.view == RADAR_VIEW_DETAIL, "input 1 -> detail");
    radar_ui_on_input(&ui, 1200); ST_CHECK(ui.view == RADAR_VIEW_STATS,  "input 2 -> stats");
    radar_ui_on_input(&ui, 1300); ST_CHECK(ui.view == RADAR_VIEW_RADAR,  "input 3 -> wraps");
    radar_ui_on_input(&ui, 2000); radar_ui_on_input(&ui, 2000);          // -> stats
    radar_ui_on_tick(&ui, 2000 + RADAR_VIEW_IDLE_MS + 1, 0);
    ST_CHECK(ui.view == RADAR_VIEW_RADAR, "idle returns to radar");
    radar_ui_on_tick(&ui, 2000 + RADAR_BL_IDLE_MS + 2, 0);
    ST_CHECK(!ui.backlight_on, "clear + idle sleeps backlight");
    radar_ui_on_tick(&ui, 999999, 1);
    ST_CHECK(ui.backlight_on && ui.view == RADAR_VIEW_RADAR, "new follower wakes + radar");
}
```

Register `test_radar_ui();` after `test_radar_geometry();`. Add `radar_ui.c` to the component `CMakeLists.txt` `SRCS`.

- [ ] **Step 3: Run to verify it fails**

`CHURN_SELFTEST 1`, `idf.py build`. Expected: link error `undefined reference to radar_ui_reset`.

- [ ] **Step 4: Write `components/simulacra_radar/radar_ui.c`**

```c
#include "radar_ui.h"

void radar_ui_reset(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count)
{
    ui->view = RADAR_VIEW_RADAR; ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms;
    ui->backlight_on = true; ui->last_threat_count = threat_count;
}
void radar_ui_on_input(radar_ui_t *ui, uint32_t now_ms)
{
    ui->view = (radar_view_t)((ui->view + 1) % RADAR_VIEW_COUNT);
    ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true;
}
void radar_ui_on_tick(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count)
{
    if (threat_count > ui->last_threat_count) {
        ui->backlight_on = true; ui->view = RADAR_VIEW_RADAR; ui->last_wake_ms = now_ms;
    }
    ui->last_threat_count = threat_count;
    if (ui->view != RADAR_VIEW_RADAR &&
        (uint32_t)(now_ms - ui->last_input_ms) >= RADAR_VIEW_IDLE_MS) ui->view = RADAR_VIEW_RADAR;
    if (threat_count == 0 && (uint32_t)(now_ms - ui->last_wake_ms) >= RADAR_BL_IDLE_MS)
        ui->backlight_on = false;
    else if (threat_count > 0) ui->backlight_on = true;
}
```

- [ ] **Step 5: Run to verify it passes**

`idf.py build && idf.py -p COM12 flash`, read `-Grep 'SELFTEST'`. Expected: `PASS (fails=0)`.

- [ ] **Step 6: Commit**

```
git add components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_ui.c components/simulacra_radar/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(espnow): shared view + backlight state machine (self-tested)"
```
Set `CHURN_SELFTEST 0` back.

---

### Task 3: Wire layer — packed status + AES-GCM seal/open + replay (pure, self-tested)

**Files:**
- Create: `components/simulacra_radar/radar_key.h`, `components/simulacra_radar/radar_wire.h`, `components/simulacra_radar/radar_wire.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `radar_wire.c`)
- Modify: `main/churn_selftest.c` (`test_radar_wire()`, register)

**Interfaces:**
- Produces: `radar_wire_status_t`; `radar_replay_t`; `int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type, const uint8_t *payload, size_t payload_len, const uint8_t key[32], const uint8_t salt[4], uint64_t counter)`; `int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32], uint8_t *out_type, uint8_t *payload, size_t *payload_len, uint8_t out_salt[4], uint64_t *out_counter)`; `bool radar_replay_ok(radar_replay_t *st, const uint8_t salt[4], uint64_t counter)`.
- Consumes: `DETECT_MAX_THREATS` from `detect.h` (the component adds `main` include path via the decoy build; the CYD app defines `DETECT_MAX_THREATS` locally — see Task 6). To keep the component self-contained, `radar_wire.h` defines its own `RADAR_MAX_THREATS 8` (matching `DETECT_MAX_THREATS`) so it needs no decoy header.

- [ ] **Step 1: Write `components/simulacra_radar/radar_key.h`**

```c
#pragma once
#include <stdint.h>
// PRE-SHARED KEY for the ESP-NOW radar link. CHANGE ME before real use — both the decoy
// and the CYD must carry the SAME 32 bytes. This placeholder is intentionally not secret.
static const uint8_t SIMULACRA_ESPNOW_KEY[32] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07, 0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17, 0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
};
```

- [ ] **Step 2: Write `components/simulacra_radar/radar_wire.h`**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RADAR_MAGIC0 0x5A
#define RADAR_MAGIC1 0x4D
#define RADAR_WIRE_VER 1
#define RADAR_TYPE_REQUEST 1
#define RADAR_TYPE_STATUS  2
#define RADAR_MAX_THREATS  8        // must match DETECT_MAX_THREATS
#define RADAR_KEY_LEN   32
#define RADAR_NONCE_LEN 12
#define RADAR_TAG_LEN   16
#define RADAR_HDR_LEN    4          // magic(2)+ver+type
#define RADAR_FRAME_MAX 250

typedef struct __attribute__((packed)) {
    uint32_t uptime_s; uint8_t flags;            // bit0 paused, bit1 config_mode
    uint16_t active_devices, roster_size; uint32_t probes_sent;
    uint16_t epoch, pop_ewma; uint32_t total_obs;
    uint8_t active_target, threat_count;
    struct __attribute__((packed)) {
        uint32_t hash; uint16_t vendor; uint8_t epochs; int8_t best_rssi;
        uint16_t first_epoch, last_epoch;
    } threats[RADAR_MAX_THREATS];
} radar_wire_status_t;

typedef struct { uint8_t salt[4]; uint64_t counter; bool seen; } radar_replay_t;

// Build [magic|ver|type|nonce|ct|tag] into frame. nonce = salt(4)|counter(8 BE). magic|ver|type
// authenticated as AAD. Returns 0 on success, <0 on error; *frame_len set to total bytes.
int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[32], const uint8_t salt[4], uint64_t counter);

// Verify + decrypt a frame. Returns 0 on success (type/payload/salt/counter filled), <0 if the
// header/magic/tag is bad. payload buffer must hold >= (frame_len - overhead) bytes.
int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32],
                    uint8_t *out_type, uint8_t *payload, size_t *payload_len,
                    uint8_t out_salt[4], uint64_t *out_counter);

// Replay gate: accept iff salt changed (peer reboot) or counter strictly newer. Updates st.
bool radar_replay_ok(radar_replay_t *st, const uint8_t salt[4], uint64_t counter);
```

- [ ] **Step 3: Write the failing test in `main/churn_selftest.c`**

Add `#include "radar_wire.h"` and `#include "radar_key.h"` and `#include <string.h>` (if not present), then above `churn_selftest_run`:

```c
static void test_radar_wire(void)
{
    radar_wire_status_t st; memset(&st, 0, sizeof st);
    st.uptime_s = 4242; st.active_devices = 7; st.threat_count = 1;
    st.threats[0].hash = 0xDEADBEEF; st.threats[0].best_rssi = -44; st.threats[0].epochs = 5;

    uint8_t salt[4] = { 0xAA,0xBB,0xCC,0xDD };
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen = 0;
    int rc = radar_wire_seal(frame, &flen, RADAR_TYPE_STATUS,
                             (uint8_t*)&st, sizeof st, SIMULACRA_ESPNOW_KEY, salt, 100);
    ST_CHECK(rc == 0 && flen == RADAR_HDR_LEN + RADAR_NONCE_LEN + sizeof(st) + RADAR_TAG_LEN,
             "seal produces a full frame");
    ST_CHECK(frame[0] == RADAR_MAGIC0 && frame[3] == RADAR_TYPE_STATUS, "header intact");

    uint8_t type, pl[RADAR_FRAME_MAX], osalt[4]; size_t plen = 0; uint64_t ctr = 0;
    rc = radar_wire_open(frame, flen, SIMULACRA_ESPNOW_KEY, &type, pl, &plen, osalt, &ctr);
    ST_CHECK(rc == 0 && type == RADAR_TYPE_STATUS && plen == sizeof(st) && ctr == 100,
             "open round-trips");
    ST_CHECK(memcmp(pl, &st, sizeof st) == 0, "payload survives round-trip");

    frame[flen - 1] ^= 0x01;                                   // tamper the tag
    ST_CHECK(radar_wire_open(frame, flen, SIMULACRA_ESPNOW_KEY, &type, pl, &plen, osalt, &ctr) < 0,
             "tampered frame rejected");

    radar_replay_t rp = {0};
    ST_CHECK(radar_replay_ok(&rp, salt, 100), "first counter accepted");
    ST_CHECK(!radar_replay_ok(&rp, salt, 100), "replay of same counter rejected");
    ST_CHECK(!radar_replay_ok(&rp, salt, 99),  "older counter rejected");
    ST_CHECK(radar_replay_ok(&rp, salt, 101),  "newer counter accepted");
    uint8_t salt2[4] = { 1,2,3,4 };
    ST_CHECK(radar_replay_ok(&rp, salt2, 1),   "reboot (new salt) resets + accepts");
}
```

Register `test_radar_wire();` after `test_radar_ui();`. Add `radar_wire.c` to the component `SRCS`.

- [ ] **Step 4: Run to verify it fails**

`CHURN_SELFTEST 1`, `idf.py build`. Expected: link error `undefined reference to radar_wire_seal`.

- [ ] **Step 5: Write `components/simulacra_radar/radar_wire.c`**

```c
#include "radar_wire.h"
#include <string.h>
#include "mbedtls/gcm.h"

static void make_nonce(uint8_t nonce[12], const uint8_t salt[4], uint64_t counter)
{
    memcpy(nonce, salt, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(counter >> (8 * (7 - i)));  // big-endian
}

int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[32], const uint8_t salt[4], uint64_t counter)
{
    if (RADAR_HDR_LEN + RADAR_NONCE_LEN + payload_len + RADAR_TAG_LEN > RADAR_FRAME_MAX) return -1;
    frame[0] = RADAR_MAGIC0; frame[1] = RADAR_MAGIC1; frame[2] = RADAR_WIRE_VER; frame[3] = type;
    uint8_t nonce[12]; make_nonce(nonce, salt, counter);
    memcpy(frame + RADAR_HDR_LEN, nonce, RADAR_NONCE_LEN);
    uint8_t *ct  = frame + RADAR_HDR_LEN + RADAR_NONCE_LEN;
    uint8_t *tag = ct + payload_len;

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, payload_len,
                          nonce, RADAR_NONCE_LEN, frame, RADAR_HDR_LEN,   // AAD = header
                          payload, ct, RADAR_TAG_LEN, tag);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;
    *frame_len = RADAR_HDR_LEN + RADAR_NONCE_LEN + payload_len + RADAR_TAG_LEN;
    return 0;
}

int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32],
                    uint8_t *out_type, uint8_t *payload, size_t *payload_len,
                    uint8_t out_salt[4], uint64_t *out_counter)
{
    if (frame_len < RADAR_HDR_LEN + RADAR_NONCE_LEN + RADAR_TAG_LEN) return -1;
    if (frame[0] != RADAR_MAGIC0 || frame[1] != RADAR_MAGIC1 || frame[2] != RADAR_WIRE_VER) return -1;
    const uint8_t *nonce = frame + RADAR_HDR_LEN;
    size_t pl = frame_len - RADAR_HDR_LEN - RADAR_NONCE_LEN - RADAR_TAG_LEN;
    const uint8_t *ct  = nonce + RADAR_NONCE_LEN;
    const uint8_t *tag = ct + pl;

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, pl, nonce, RADAR_NONCE_LEN,
                          frame, RADAR_HDR_LEN, tag, RADAR_TAG_LEN, ct, payload);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;                             // bad tag / bad key
    *out_type = frame[3]; *payload_len = pl;
    memcpy(out_salt, nonce, 4);
    uint64_t c = 0; for (int i = 0; i < 8; i++) c = (c << 8) | nonce[4 + i];
    *out_counter = c;
    return 0;
}

bool radar_replay_ok(radar_replay_t *st, const uint8_t salt[4], uint64_t counter)
{
    if (!st->seen || memcmp(st->salt, salt, 4) != 0) {     // fresh or peer rebooted
        memcpy(st->salt, salt, 4); st->counter = counter; st->seen = true; return true;
    }
    if (counter > st->counter) { st->counter = counter; return true; }
    return false;
}
```

- [ ] **Step 6: Run to verify it passes**

`idf.py build && idf.py -p COM12 flash`, read `-Grep 'SELFTEST'`. Expected: `PASS (fails=0)`.

- [ ] **Step 7: Commit**

```
git add components/simulacra_radar/radar_key.h components/simulacra_radar/radar_wire.h components/simulacra_radar/radar_wire.c components/simulacra_radar/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(espnow): AES-GCM wire layer — packed status, seal/open, replay (self-tested)"
```
Set `CHURN_SELFTEST 0` back.

---

### Task 4: webui_status_t → radar_wire_status_t converter (pure, self-tested)

**Files:**
- Create: `main/esp_now_link.h`, `main/esp_now_link.c` (converter only for now)
- Modify: `main/CMakeLists.txt` (add `esp_now_link.c` to `SRCS`)
- Modify: `main/churn_selftest.c` (`test_espnow_convert()`, register)

**Interfaces:**
- Produces: `void espnow_status_from_webui(radar_wire_status_t *out, const webui_status_t *in)`; `void esp_now_link_start(void)` (declared now, defined in Task 5).
- Consumes: `webui_status_t` (`webui.h`), `radar_wire_status_t` (`radar_wire.h`).

- [ ] **Step 1: Write `main/esp_now_link.h`**

```c
#pragma once
#include "webui.h"        // webui_status_t
#include "radar_wire.h"   // radar_wire_status_t

// Pack a live decoy snapshot into the wire view-model (pure; unit-tested).
void espnow_status_from_webui(radar_wire_status_t *out, const webui_status_t *in);

// Init ESP-NOW responder + start its task (defined in Task 5). Gated by SIMULACRA_ESPNOW.
void esp_now_link_start(void);
```

- [ ] **Step 2: Write the failing test in `main/churn_selftest.c`**

Add `#include "esp_now_link.h"`, then:

```c
static void test_espnow_convert(void)
{
    webui_status_t w; memset(&w, 0, sizeof w);
    w.uptime_s = 61; w.decoy_paused = true; w.active_devices = 6; w.roster_size = 64;
    w.probes_sent = 2048; w.epoch = 3; w.pop_ewma = 9; w.total_obs = 500;
    w.active_target = 6; w.threat_count = 1;
    w.threats[0].hash = 0xC0FFEE; w.threats[0].vendor = 0x004C;
    w.threats[0].best_rssi = -50; w.threats[0].epochs = 4;

    radar_wire_status_t r; espnow_status_from_webui(&r, &w);
    ST_CHECK(r.uptime_s == 61 && r.active_devices == 6 && r.probes_sent == 2048,
             "scalars copied");
    ST_CHECK((r.flags & 0x1) != 0, "paused flag packed");
    ST_CHECK(r.threat_count == 1 && r.threats[0].hash == 0xC0FFEE &&
             r.threats[0].best_rssi == -50, "threat copied hash-only");
}
```

Register `test_espnow_convert();` after `test_radar_wire();`. Add `esp_now_link.c` to `main/CMakeLists.txt` `SRCS`.

- [ ] **Step 3: Run to verify it fails**

`CHURN_SELFTEST 1`, `idf.py build`. Expected: link error `undefined reference to espnow_status_from_webui`.

- [ ] **Step 4: Write the converter in `main/esp_now_link.c`**

```c
#include "esp_now_link.h"
#include <string.h>

void espnow_status_from_webui(radar_wire_status_t *out, const webui_status_t *in)
{
    memset(out, 0, sizeof(*out));
    out->uptime_s = in->uptime_s;
    out->flags = (uint8_t)((in->decoy_paused ? 0x1 : 0) | (in->wifi_config_mode ? 0x2 : 0));
    out->active_devices = in->active_devices; out->roster_size = in->roster_size;
    out->probes_sent = in->probes_sent; out->epoch = in->epoch; out->pop_ewma = in->pop_ewma;
    out->total_obs = in->total_obs; out->active_target = in->active_target;
    uint8_t n = in->threat_count; if (n > RADAR_MAX_THREATS) n = RADAR_MAX_THREATS;
    out->threat_count = n;
    for (uint8_t i = 0; i < n; i++) {
        out->threats[i].hash = in->threats[i].hash;
        out->threats[i].vendor = in->threats[i].vendor;
        out->threats[i].epochs = in->threats[i].epochs;
        out->threats[i].best_rssi = in->threats[i].best_rssi;
        out->threats[i].first_epoch = in->threats[i].first_epoch;
        out->threats[i].last_epoch = in->threats[i].last_epoch;
    }
}
```

- [ ] **Step 5: Run to verify it passes**

`idf.py build && idf.py -p COM12 flash`, read `-Grep 'SELFTEST'`. Expected: `PASS (fails=0)`.

- [ ] **Step 6: Commit**

```
git add main/esp_now_link.h main/esp_now_link.c main/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(espnow): webui->wire status converter (self-tested)"
```
Set `CHURN_SELFTEST 0` back.

---

### Task 5: Decoy ESP-NOW responder + gate + boot wiring (on-target)

**Files:**
- Modify: `main/esp_now_link.c` (ESP-NOW init, RX callback, response task, MAC randomization)
- Modify: `main/CMakeLists.txt` (add `esp_now` to `REQUIRES`)
- Modify: `main/simulacra_main.c` (`SIMULACRA_ESPNOW` gate + `esp_now_link_start()`)

**Interfaces:**
- Consumes: `espnow_status_from_webui`, `webui_gather_status`, `radar_wire_seal`, `radar_replay_ok`, `SIMULACRA_ESPNOW_KEY`, ESP-NOW + esp_wifi APIs.
- Produces: `esp_now_link_start()` (final form).

- [ ] **Step 1: Implement the responder in `main/esp_now_link.c`**

Append below the converter:

```c
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radar_key.h"

#ifndef SIMULACRA_ESPNOW_CHANNEL
#define SIMULACRA_ESPNOW_CHANNEL 1
#endif
static const char *ETAG = "espnow";
static const uint8_t BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static uint8_t   s_salt[4];
static uint64_t  s_counter;
static radar_replay_t s_req_replay;                 // reject replayed requests
static volatile bool  s_answer;                     // set by RX cb, consumed by task

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    uint8_t type, pl[RADAR_FRAME_MAX], salt[4]; size_t plen; uint64_t ctr;
    if (radar_wire_open(data, (size_t)len, SIMULACRA_ESPNOW_KEY, &type, pl, &plen, salt, &ctr) != 0)
        return;                                     // not ours / bad tag
    if (type != RADAR_TYPE_REQUEST) return;
    if (!radar_replay_ok(&s_req_replay, salt, ctr)) return;   // replayed request
    s_answer = true;                                // defer the heavy work to the task
}

static void respond_once(void)
{
    webui_status_t w; webui_gather_status(&w);
    radar_wire_status_t r; espnow_status_from_webui(&r, &w);
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    if (radar_wire_seal(frame, &flen, RADAR_TYPE_STATUS, (uint8_t*)&r, sizeof r,
                        SIMULACRA_ESPNOW_KEY, s_salt, ++s_counter) != 0) return;
    for (int i = 0; i < 3; i++) esp_now_send(BCAST, frame, flen);   // 3x back-to-back
    ESP_LOGW(ETAG, "answered request (%u B x3)", (unsigned)flen);
}

static void espnow_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_answer) { s_answer = false; respond_once(); }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void esp_now_link_start(void)
{
    // Wi-Fi is already up (coexist STA). Randomize the STA source MAC once (locally-administered).
    uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_fill_random(mac, 6); mac[0] = (mac[0] & 0xFE) | 0x02;      // LAA, unicast
    esp_wifi_set_mac(WIFI_IF_STA, mac);                            // best-effort; ignore rc
    esp_fill_random(s_salt, sizeof s_salt);

    if (esp_now_init() != ESP_OK) { ESP_LOGE(ETAG, "esp_now_init failed"); return; }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BCAST, 6); peer.channel = SIMULACRA_ESPNOW_CHANNEL; peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
    esp_now_register_recv_cb(on_recv);
    xTaskCreate(espnow_task, "espnow", 4096, NULL, 3, NULL);
    ESP_LOGW(ETAG, "responder up (ch=%d, listen-only until requested)", SIMULACRA_ESPNOW_CHANNEL);
}
```

Add `esp_now` to `main/CMakeLists.txt` `REQUIRES`.

Note on MAC/channel: `esp_wifi_set_mac` may require the STA stopped on some IDF versions; if it returns `ESP_ERR_WIFI_MODE`/`_IF`, skip it (log + continue with the factory MAC) — the opsec model still holds (broadcast + on-demand). The decoy keeps hopping for probes; `on_recv` fires only while on `SIMULACRA_ESPNOW_CHANNEL`.

- [ ] **Step 2: Gate it into boot in `main/simulacra_main.c`**

Add the gate after the `SIMULACRA_DISPLAY` block:
```c
// Remote ESP-NOW radar link (answers a CYD's telemetry requests). Default 0.
#ifndef SIMULACRA_ESPNOW
#define SIMULACRA_ESPNOW 0
#endif
```
Ensure `#include "esp_now_link.h"` is present. In `simulacra_task`, after the `#if SIMULACRA_WEBUI` block (so Wi-Fi STA is up via `coexist_set_wifi_enabled(true)`), add:
```c
#if SIMULACRA_ESPNOW
    esp_now_link_start();   // listen-only responder; answers CYD requests over ESP-NOW
#endif
```

- [ ] **Step 3: Build clean with the link OFF (regression)**

```
idf.py build
```
Expected: clean build; no `esp_now` symbols pulled into the default image beyond the link dependency.

- [ ] **Step 4: Build + flash a link-ON decoy (SparkFun C6, COM13)**

```
idf.py -DSIMULACRA_ESPNOW=1 build && idf.py -DSIMULACRA_ESPNOW=1 -p COM13 flash
```
Expected serial: `responder up (ch=1, listen-only until requested)`, and the decoy continues `decoy alive active=N` + `burst ch=...`. No `answered request` yet (no CYD). This confirms the responder inits and stays silent. Full request/response is verified in Task 8.

- [ ] **Step 5: Commit**

```
git add main/esp_now_link.c main/CMakeLists.txt main/simulacra_main.c
git commit -m "feat(espnow): decoy responder — listen-only, encrypted, on-demand + boot gate"
```

---

### Task 6: CYD app scaffold + panel bring-up (on-target)

**Files:**
- Create: `cyd/CMakeLists.txt`, `cyd/sdkconfig.defaults`, `cyd/main/CMakeLists.txt`, `cyd/main/cyd_main.c`, `cyd/main/idf_component.yml`

**Interfaces:**
- Produces: a flashable CYD app that shows a test pattern; `static bool cyd_panel_init(esp_lcd_panel_handle_t *out)`.

- [ ] **Step 1: Probe the CYD display controller**

The ESP32-2432S028 ships as ILI9341 or ST7789. Flash the vendor's display test (or an `esp_lcd` ILI9341 test-pattern build) once; if colors are garbage/mirrored, it's the other controller. Record the result and pick the matching `esp_lcd_new_panel_*` call below. Known CYD pins: `MOSI=13, SCK=14, CS=15, DC=2, RST=-1(tie high), BL=21` (display SPI = HSPI/SPI2); touch on a *separate* bus `T_CLK=25, T_CS=33, T_DIN=32, T_DOUT=39, T_IRQ=36`.

- [ ] **Step 2: Write the project files**

`cyd/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(simulacra_cyd)
```
`cyd/sdkconfig.defaults`:
```
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
```
`cyd/main/idf_component.yml` (managed panel + touch drivers):
```yaml
dependencies:
  espressif/esp_lcd_ili9341: "^1.2.0"
  espressif/esp_lcd_touch_xpt2046: "^1.0.0"
```
`cyd/main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "cyd_main.c"
    INCLUDE_DIRS "."
    REQUIRES simulacra_radar esp_lcd esp_wifi esp_now nvs_flash mbedtls driver esp_timer
)
```

- [ ] **Step 3: Write `cyd/main/cyd_main.c` (panel + test pattern)**

```c
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"        // swap to esp_lcd_panel_st7789 if Step 1 says ST7789
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs_flash.h"

#define PIN_MOSI 13
#define PIN_SCK  14
#define PIN_CS   15
#define PIN_DC    2
#define PIN_RST  (-1)
#define PIN_BL   21
#define LCD_W    240
#define LCD_H    320
static const char *TAG = "cyd";
static esp_lcd_panel_handle_t s_panel;

static bool cyd_panel_init(esp_lcd_panel_handle_t *out)
{
    ledc_timer_config_t lt = { .speed_mode=LEDC_LOW_SPEED_MODE, .duty_resolution=LEDC_TIMER_8_BIT,
                               .timer_num=LEDC_TIMER_0, .freq_hz=5000, .clk_cfg=LEDC_AUTO_CLK };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = { .gpio_num=PIN_BL, .speed_mode=LEDC_LOW_SPEED_MODE,
                                 .channel=LEDC_CHANNEL_0, .timer_sel=LEDC_TIMER_0, .duty=255 };
    ledc_channel_config(&lc);

    spi_bus_config_t bus = { .mosi_io_num=PIN_MOSI, .sclk_io_num=PIN_SCK, .miso_io_num=-1,
                             .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=LCD_W*40*2+8 };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = { .dc_gpio_num=PIN_DC, .cs_gpio_num=PIN_CS,
        .pclk_hz=40*1000*1000, .lcd_cmd_bits=8, .lcd_param_bits=8, .spi_mode=0, .trans_queue_depth=10 };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io) != ESP_OK) return false;
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num=PIN_RST, .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_BGR,
                                      .bits_per_pixel=16 };
    if (esp_lcd_new_panel_ili9341(io, &pc, out) != ESP_OK) return false;   // or _st7789
    esp_lcd_panel_reset(*out); esp_lcd_panel_init(*out);
    esp_lcd_panel_invert_color(*out, false);       // flip if colors look inverted
    esp_lcd_panel_disp_on_off(*out, true);
    return true;
}

void app_main(void)
{
    nvs_flash_init();
    if (!cyd_panel_init(&s_panel)) { ESP_LOGE(TAG, "panel init failed"); return; }
    static uint16_t band[LCD_W * 40];
    for (int i = 0; i < LCD_W * 40; i++) band[i] = 0xF800;      // red
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, 40, band);
    for (int i = 0; i < LCD_W * 40; i++) band[i] = 0x07E0;      // green
    esp_lcd_panel_draw_bitmap(s_panel, 0, 40, LCD_W, 80, band);
    ESP_LOGW(TAG, "panel up: test bands drawn");
}
```

- [ ] **Step 4: Build + flash the CYD (esp32, COM14, IDF 5.4)**

From `cyd/`:
```
idf.py set-target esp32
idf.py build && idf.py -p COM14 flash
```
Expected serial: `panel up: test bands drawn`. Expected panel: a red band then a green band. If colors are swapped/inverted or the image is mirrored, adjust `rgb_ele_order` (RGB/BGR), `esp_lcd_panel_invert_color`, or add `esp_lcd_panel_mirror`; if garbage, switch to `esp_lcd_new_panel_st7789` (Step 1).

- [ ] **Step 5: Commit**

```
git add cyd/
git commit -m "feat(espnow): CYD app scaffold + ST7789/ILI9341 panel bring-up"
```

---

### Task 7: Shared gfx primitives + renderers + CYD static render (on-target)

**Files:**
- Create: `components/simulacra_radar/radar_gfx.h`, `radar_gfx.c`, `radar_font.h`, `radar_render.h`, `radar_render.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `radar_gfx.c radar_render.c`)
- Modify: `cyd/main/cyd_main.c` (render a static `radar_wire_status_t` via the shared renderer)

**Interfaces:**
- Produces: `radar_gfx_t`; primitives `radar_gfx_clear/pixel/hline/vline/line/fill_rect/circle/text`; `typedef void (*radar_flush_fn)(int y0, int h, const uint16_t *buf, void *ctx)`; `void radar_render_view(radar_view_t view, const radar_wire_status_t *st, uint16_t sweep_deg, uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx)`.
- Consumes: `radar_geom.h`, `radar_wire.h`.

- [ ] **Step 1: Write `components/simulacra_radar/radar_gfx.h` + `radar_font.h`**

`radar_gfx.h`:
```c
#pragma once
#include <stdint.h>
typedef struct { uint16_t *buf; int w; int y0; int h; } radar_gfx_t;   // band = rows [y0, y0+h)
void radar_gfx_clear(radar_gfx_t *g, uint16_t c);
void radar_gfx_pixel(radar_gfx_t *g, int x, int y, uint16_t c);
void radar_gfx_hline(radar_gfx_t *g, int x0, int x1, int y, uint16_t c);
void radar_gfx_vline(radar_gfx_t *g, int x, int y0, int y1, uint16_t c);
void radar_gfx_line(radar_gfx_t *g, int x0, int y0, int x1, int y1, uint16_t c);
void radar_gfx_fill_rect(radar_gfx_t *g, int x0, int y0, int w, int h, uint16_t c);
void radar_gfx_circle(radar_gfx_t *g, int cx, int cy, int r, uint16_t c);
void radar_gfx_text(radar_gfx_t *g, int x, int y, const char *s, uint16_t c);
```
`radar_font.h`: an 8×8 ASCII font `static const uint8_t RADAR_FONT8X8[96][8]` (0x20–0x7F, row-major, bit i of row r = pixel (i,r)). Drop in the complete public-domain `font8x8_basic` table.

- [ ] **Step 2: Write `components/simulacra_radar/radar_gfx.c`**

```c
#include "radar_gfx.h"
#include "radar_font.h"
#include <stdlib.h>

void radar_gfx_clear(radar_gfx_t *g, uint16_t c){ for(int i=0;i<g->w*g->h;i++) g->buf[i]=c; }
void radar_gfx_pixel(radar_gfx_t *g, int x, int y, uint16_t c){
    if(x<0||x>=g->w) return; int ry=y-g->y0; if(ry<0||ry>=g->h) return; g->buf[ry*g->w+x]=c; }
void radar_gfx_hline(radar_gfx_t *g,int x0,int x1,int y,uint16_t c){ for(int x=x0;x<=x1;x++) radar_gfx_pixel(g,x,y,c); }
void radar_gfx_vline(radar_gfx_t *g,int x,int y0,int y1,uint16_t c){ for(int y=y0;y<=y1;y++) radar_gfx_pixel(g,x,y,c); }
void radar_gfx_fill_rect(radar_gfx_t *g,int x0,int y0,int w,int h,uint16_t c){
    for(int y=y0;y<y0+h;y++) for(int x=x0;x<x0+w;x++) radar_gfx_pixel(g,x,y,c); }
void radar_gfx_line(radar_gfx_t *g,int x0,int y0,int x1,int y1,uint16_t c){
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
    for(;;){ radar_gfx_pixel(g,x0,y0,c); if(x0==x1&&y0==y1)break; int e2=2*err;
        if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} } }
void radar_gfx_circle(radar_gfx_t *g,int cx,int cy,int r,uint16_t c){
    int x=r,y=0,e=1-r; while(x>=y){
        radar_gfx_pixel(g,cx+x,cy+y,c);radar_gfx_pixel(g,cx+y,cy+x,c);radar_gfx_pixel(g,cx-y,cy+x,c);radar_gfx_pixel(g,cx-x,cy+y,c);
        radar_gfx_pixel(g,cx-x,cy-y,c);radar_gfx_pixel(g,cx-y,cy-x,c);radar_gfx_pixel(g,cx+y,cy-x,c);radar_gfx_pixel(g,cx+x,cy-y,c);
        y++; if(e<0)e+=2*y+1; else{x--;e+=2*(y-x)+1;} } }
static void gfx_char(radar_gfx_t *g,int x,int y,char ch,uint16_t c){
    if(ch<0x20||ch>0x7F) ch='?'; const uint8_t *gl=RADAR_FONT8X8[ch-0x20];
    for(int r=0;r<8;r++) for(int col=0;col<8;col++) if(gl[r]&(1<<col)) radar_gfx_pixel(g,x+col,y+r,c); }
void radar_gfx_text(radar_gfx_t *g,int x,int y,const char *s,uint16_t c){
    for(; *s; s++, x+=8) gfx_char(g,x,y,*s,c); }
```

- [ ] **Step 3: Write `components/simulacra_radar/radar_render.{h,c}`**

`radar_render.h`:
```c
#pragma once
#include "radar_ui.h"
#include "radar_wire.h"
typedef void (*radar_flush_fn)(int y0, int h, const uint16_t *buf, void *ctx);
// Banded full-frame render of `view` from `st` (sweep_deg animates the radar). `band` is a
// scratch buffer of w*band_h uint16; flush() pushes each band to the panel.
void radar_render_view(radar_view_t view, const radar_wire_status_t *st, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
```
`radar_render.c` (renders the three views using `radar_geom` + `radar_gfx`; colors RGB565; radar center + rings + sweep + follower dots + banner + a stats strip; detail rows; stats rows — mirrors the on-board display design):
```c
#include "radar_render.h"
#include "radar_gfx.h"
#include "radar_geom.h"
#include <stdio.h>
#define COL_BG 0x0000
#define COL_FG 0xFFFF
#define COL_DIM 0x7BEF
#define COL_RING 0x2965
#define COL_OK 0x07E0
#define COL_WARN 0xF800
#define COL_SWEEP 0x0400
#define RCX 120
#define RCY 120
#define RR 100

static uint16_t threat_color(uint8_t ep){ return ep>=5?COL_WARN:(ep>=2?0xFD20:0xFFE0); }

static void draw_radar(radar_gfx_t *g, const radar_wire_status_t *st, uint16_t sweep){
    radar_gfx_circle(g,RCX,RCY,RR,COL_RING); radar_gfx_circle(g,RCX,RCY,RR*2/3,COL_RING);
    radar_gfx_circle(g,RCX,RCY,RR/3,COL_RING);
    radar_gfx_hline(g,RCX-RR,RCX+RR,RCY,COL_RING); radar_gfx_vline(g,RCX,RCY-RR,RCY+RR,COL_RING);
    int sx,sy; radar_polar_to_xy(RCX,RCY,RR,sweep,&sx,&sy); radar_gfx_line(g,RCX,RCY,sx,sy,COL_SWEEP);
    for(uint8_t i=0;i<st->threat_count;i++){
        uint16_t rr=radar_rssi_to_radius(st->threats[i].best_rssi,RR/4,RR);
        uint16_t an=radar_hash_to_angle(st->threats[i].hash);
        int x,y; radar_polar_to_xy(RCX,RCY,rr,an,&x,&y);
        radar_gfx_fill_rect(g,x-2,y-2,5,5,threat_color(st->threats[i].epochs)); }
    char b[24];
    if(st->threat_count==0) radar_gfx_text(g,84,250,"CLEAR",COL_OK);
    else { snprintf(b,sizeof b,"! %u FOLLOWERS",(unsigned)st->threat_count); radar_gfx_text(g,40,250,b,COL_WARN); }
    char l[40]; snprintf(l,sizeof l,"decoys %u  up %lus",(unsigned)st->active_devices,(unsigned long)st->uptime_s);
    radar_gfx_text(g,10,296,l,COL_DIM);
}
static void draw_detail(radar_gfx_t *g, const radar_wire_status_t *st){
    radar_gfx_text(g,8,6,"FOLLOWERS",COL_FG);
    if(st->threat_count==0){ radar_gfx_text(g,8,30,"none confirmed",COL_DIM); return; }
    for(uint8_t i=0;i<st->threat_count;i++){ char r[40];
        snprintf(r,sizeof r,"%08lx v%04x %ddB %uep",(unsigned long)st->threats[i].hash,
                 (unsigned)st->threats[i].vendor,(int)st->threats[i].best_rssi,(unsigned)st->threats[i].epochs);
        radar_gfx_text(g,6,30+i*18,r,threat_color(st->threats[i].epochs)); } }
static void draw_stats(radar_gfx_t *g, const radar_wire_status_t *st){
    char l[40]; int y=6; radar_gfx_text(g,8,y,"DECOY / POP",COL_FG); y+=24;
    #define ROW(...) do{ snprintf(l,sizeof l,__VA_ARGS__); radar_gfx_text(g,6,y,l,COL_DIM); y+=18; }while(0)
    ROW("decoys %u/%u tgt %u",(unsigned)st->active_devices,(unsigned)st->roster_size,(unsigned)st->active_target);
    ROW("pop %u obs %lu",(unsigned)st->pop_ewma,(unsigned long)st->total_obs);
    ROW("epoch %u probes %lu",(unsigned)st->epoch,(unsigned long)st->probes_sent);
    ROW("churn %s",(st->flags&0x1)?"PAUSED":"run");
    ROW("up %lus",(unsigned long)st->uptime_s);
    #undef ROW
}
void radar_render_view(radar_view_t view, const radar_wire_status_t *st, uint16_t sweep,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx){
    for(int y0=0;y0<h;y0+=band_h){ radar_gfx_t g={ .buf=band, .w=w, .y0=y0, .h=band_h };
        radar_gfx_clear(&g,COL_BG);
        if(view==RADAR_VIEW_DETAIL) draw_detail(&g,st);
        else if(view==RADAR_VIEW_STATS) draw_stats(&g,st);
        else draw_radar(&g,st,sweep);
        flush(y0, band_h, band, ctx); }
}
```
Add `radar_gfx.c radar_render.c` to the component `SRCS`.

- [ ] **Step 4: Render a static status on the CYD**

In `cyd/main/cyd_main.c` `app_main`, replace the test-band block with:
```c
    static uint16_t band[LCD_W * 40];
    radar_wire_status_t demo = {0};
    demo.active_devices = 7; demo.uptime_s = 83; demo.threat_count = 2;
    demo.threats[0].hash = 0x1234; demo.threats[0].best_rssi = -41; demo.threats[0].epochs = 6;
    demo.threats[1].hash = 0x9abc; demo.threats[1].best_rssi = -66; demo.threats[1].epochs = 2;
    extern void cyd_flush(int,int,const uint16_t*,void*);
    radar_render_view(RADAR_VIEW_RADAR, &demo, 45, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
```
And add the flush + include:
```c
#include "radar_render.h"
void cyd_flush(int y0, int h, const uint16_t *buf, void *ctx){
    (void)ctx; esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_W, y0 + h, (void*)buf);
}
```

- [ ] **Step 5: Build + flash + verify (CYD, COM14)**

```
idf.py build && idf.py -p COM14 flash
```
Expected panel: the radar circle with rings + crosshair + a 45° sweep line, `! 2 FOLLOWERS` banner, two dots at different rings (the -41 dBm dot nearer center than the -66), and the `decoys 7 up 83s` strip. Text legible. Tune colors/mirror if needed.

- [ ] **Step 6: Commit**

```
git add components/simulacra_radar/radar_gfx.h components/simulacra_radar/radar_gfx.c components/simulacra_radar/radar_font.h components/simulacra_radar/radar_render.h components/simulacra_radar/radar_render.c components/simulacra_radar/CMakeLists.txt cyd/main/cyd_main.c
git commit -m "feat(espnow): shared gfx primitives + radar renderers + CYD static render"
```

---

### Task 8: CYD touch + ESP-NOW receiver + live radar (on-target)

**Files:**
- Modify: `cyd/main/cyd_main.c` (Wi-Fi/ESP-NOW init, RX + decrypt, REQUEST on touch, XPT2046 touch, render loop, link-freshness)

**Interfaces:**
- Consumes: `radar_wire_open`, `radar_wire_seal`, `radar_replay_ok`, `radar_ui_*`, `radar_render_view`, `SIMULACRA_ESPNOW_KEY`, ESP-NOW + XPT2046 APIs.

- [ ] **Step 1: Add Wi-Fi + ESP-NOW receiver + request-on-touch**

Add to `cyd/main/cyd_main.c` (init Wi-Fi STA locked to channel 1, register RX, send REQUEST). Key logic:
```c
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "radar_wire.h"
#include "radar_key.h"
#include "radar_ui.h"
#include <string.h>

#define ESPNOW_CH 1
static const uint8_t BCAST[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static radar_wire_status_t s_status;             // last good status
static volatile uint32_t   s_status_ms;          // when it arrived (0 = never)
static radar_replay_t      s_replay;
static uint8_t  s_salt[4]; static uint64_t s_ctr;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len){
    uint8_t type, pl[RADAR_FRAME_MAX], salt[4]; size_t plen; uint64_t ctr;
    if (radar_wire_open(data,(size_t)len,SIMULACRA_ESPNOW_KEY,&type,pl,&plen,salt,&ctr)!=0) return;
    if (type!=RADAR_TYPE_STATUS || plen!=sizeof(radar_wire_status_t)) return;
    if (!radar_replay_ok(&s_replay,salt,ctr)) return;
    memcpy(&s_status, pl, sizeof s_status);
    s_status_ms = (uint32_t)(esp_timer_get_time()/1000);
}
static void send_request(void){
    uint8_t nonce[4]; esp_fill_random(nonce,4);
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    if (radar_wire_seal(frame,&flen,RADAR_TYPE_REQUEST,nonce,4,SIMULACRA_ESPNOW_KEY,s_salt,++s_ctr)==0)
        for (int i=0;i<4;i++) esp_now_send(BCAST,frame,flen);
}
static void net_init(void){
    esp_netif_init(); esp_event_loop_create_default();
    wifi_init_config_t c=WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&c);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start();
    esp_wifi_set_channel(ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
    esp_now_init();
    esp_now_peer_info_t p={0}; memcpy(p.peer_addr,BCAST,6); p.channel=ESPNOW_CH; p.ifidx=WIFI_IF_STA;
    esp_now_add_peer(&p); esp_now_register_recv_cb(on_recv);
    esp_fill_random(s_salt,4);
}
```

- [ ] **Step 2: Add XPT2046 touch init + a touched() helper**

Initialize the XPT2046 on its separate SPI bus (SPI3_HOST, pins from Task 6 Step 1) via the `esp_lcd_touch_xpt2046` component; expose `static bool touched(void)` that returns true on a fresh press (debounced ~200 ms). (Use the component's `esp_lcd_touch_read_data` + `esp_lcd_touch_get_coordinates`; any valid touch counts as an input — we don't need coordinates, just the press.)

- [ ] **Step 3: Write the render loop in `app_main`**

Replace the static-render block with the live loop:
```c
    net_init();
    radar_ui_t ui; radar_ui_reset(&ui, (uint32_t)(esp_timer_get_time()/1000), 0);
    static uint16_t band[LCD_W*40]; uint16_t sweep=0; uint32_t last_req=0;
    for(;;){
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        if (touched()) { radar_ui_on_input(&ui, now); send_request(); last_req=now; }
        // keep asking every ~3s while the screen is awake so data stays fresh
        if (ui.backlight_on && now-last_req > 3000) { send_request(); last_req=now; }
        radar_ui_on_tick(&ui, now, s_status.threat_count);
        if (ui.backlight_on){
            radar_render_view(ui.view, &s_status, sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
            sweep=(uint16_t)((sweep+12)%360);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
```
Add a freshness overlay in the radar view: if `s_status_ms==0` show `SEARCHING…`; if `now-s_status_ms>5000` show `NO DECOY` (dim). (Draw it in `draw_radar` via an extra arg, or as a post-render band on the CYD side — keep it CYD-side to avoid touching the shared renderer: after `radar_render_view`, if stale, overlay one band with the text.)

- [ ] **Step 4: Build + flash + verify against the live decoy**

Ensure a decoy is running the responder (Task 5: `-DSIMULACRA_ESPNOW=1` on COM13). Then flash the CYD:
```
idf.py build && idf.py -p COM14 flash
```
Expected: CYD shows `SEARCHING…`, then on touch (or within ~3 s) the radar populates from the live decoy — `decoys N` matches the decoy's serial `active=N`; decoy serial prints `answered request`. Tap cycles radar → detail → stats → radar; idle returns to radar; leaving it clear ~30 s blanks the backlight; a tap wakes it. Pull decoy power → within ~5 s the CYD shows `NO DECOY`.

- [ ] **Step 5: Commit**

```
git add cyd/main/cyd_main.c
git commit -m "feat(espnow): CYD receiver — touch, ESP-NOW RX + decrypt, live radar + freshness"
```

---

### Task 9: End-to-end acceptance + opsec verification (on-target)

**Files:** none (verification + any small fixes uncovered).

- [ ] **Step 1: Full-loop acceptance (CYD COM14 + decoy)**

With the decoy on COM13 (`-DSIMULACRA_ESPNOW=1`) and the CYD on COM14: confirm the whole chain against a real follower. Walk a known tracker/phone into range until the decoy confirms it (serial `THREAT confirmed`); the CYD radar must (a) wake if asleep, (b) show `! 1 FOLLOWERS`, and (c) plot a dot at the ring matching its RSSI. Cycle to the follower-detail view and confirm the hash/vendor/rssi/epochs match the decoy's serial (hash-only — no MAC on screen).

- [ ] **Step 2: Opsec verification**

Run a second board in `SIMULACRA_SNIFF` (or a promiscuous capture) on channel 1. Confirm: with the CYD **not** requesting (screen asleep/away), the decoy emits **no** STATUS frames (listen-only); frames appear only after a CYD request; captured STATUS frames are ciphertext (not plaintext status); and the decoy's ESP-NOW source MAC is locally-administered (bit 0x02 of the first octet set), not the factory MAC.

- [ ] **Step 3: Coexistence + stability**

Over a 10-minute run with the CYD polling every ~3 s: the decoy keeps churning (`decoy alive active=N`) and injecting (`burst ch=... tx_rc=0`), no task watchdog, and the CYD radar stays fresh (no `NO DECOY` while in range). Note first-response latency (expect a few seconds worst-case).

- [ ] **Step 4: Commit any fixes, then update the ledger**

Commit any small fixes found (`fix(espnow): ...`). No functional commit if clean.

---

## Self-review notes

- **Spec coverage:** shared component (Tasks 1–3, 7), receiver-only rationale (design; CYD app is receive-only, Task 6/8), CYD-polls/decoy-answers trigger (Task 5 responder + Task 8 request), ESP-IDF toolchain (Task 6), broadcast + fixed ch1 (Tasks 5, 8), AES-GCM + PSK + replay (Task 3, used in 5/8), one-frame payload (Task 3 packed struct), opsec silent/broadcast/random-MAC (Task 5 + verified Task 9), gating `SIMULACRA_ESPNOW` default 0 + regression (Task 5 Steps 2–3), self-tested-core vs on-target (Tasks 1–4 self-test; 5–9 on-target), Law-3 hash-only (Task 4 converter + Task 9 check), reuse of `webui_gather_status` (Tasks 4–5). All covered.
- **Placeholder-free:** every code step shows real code. Two vendored/hardware items are flagged with concrete handling, not vague: the 8×8 font table (drop in the complete public-domain `font8x8_basic`, indexing shown) and the CYD controller/pins (concrete candidate pins + a probe-and-adjust step). Touch init (Task 8 Step 2) names the exact component + calls to use.
- **Type consistency:** `radar_wire_status_t`, `radar_replay_t`, `radar_ui_t`/`radar_view_t`, `radar_*` geometry, `radar_gfx_*`, `radar_render_view`, `espnow_status_from_webui`, and the seal/open signatures are identical across defining and consuming tasks.
- **Adjustment to the parked display plan:** the pure geometry + FSM (its Tasks 1–2) and the gfx/renderers now live in `components/simulacra_radar/` under `radar_*` names; when the on-board wired-panel display is built it consumes this component instead of re-implementing them.
- **Known on-target unknowns (resolved at bring-up, not placeholders):** CYD display controller (ILI9341 vs ST7789) + color order/mirror; `esp_wifi_set_mac` acceptance while STA started (documented fallback to factory MAC); XPT2046 calibration.
```
