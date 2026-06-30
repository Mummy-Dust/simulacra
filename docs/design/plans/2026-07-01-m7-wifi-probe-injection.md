# M7 Wi-Fi Probe-Request Injection Implementation Plan



**Goal:** A Wi-Fi-only build mode that injects synthetic, randomized-MAC, broadcast probe-request
frames at a plausible phone-like rate across channels (2.4 GHz everywhere; +5 GHz on the C5), so a
Wi-Fi sniffer sees believable phone scanning that corresponds to no real device.

**Architecture:** New `probe.{h,c}` owns the probe-request frame builder, randomized
locally-administered MACs, Wi-Fi raw-TX init, and the injection loop (an active set of fake "phones"
rotating over time, channel-hopping). A `SIMULACRA_PROBE` build mode in `simulacra_main.c` runs it
with BLE/NimBLE not initialized. BLE+Wi-Fi coexistence is M8.

**Tech Stack:** ESP-IDF v5.5 (C5), `esp_wifi_80211_tx`, C. Verified by the looped on-target
self-test (frame logic) + serial TX-rc liveness + a user-side Wi-Fi sniffer. Default build = BLE decoy.

**Spec:** `docs/design/specs/2026-07-01-m7-wifi-probe-injection-design.md`

**Verified APIs (IDF 5.5):** `esp_wifi_80211_tx(WIFI_IF_STA, buf, len(24..1500), en_sys_seq=true)`
supports probe-request frames; `esp_wifi_set_channel(primary, WIFI_SECOND_CHAN_NONE)` accepts 5 GHz
channels when band mode allows; `esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO)` = 2.4+5 GHz (C5).

## Global Constraints

- **Wi-Fi Law 3:** broadcast/wildcard-SSID probes ONLY (SSID IE id 0, **len 0**); never a directed
  probe naming an SSID. Hardcoded in the builder + self-test-guarded.
- **Randomized locally-administered MACs:** `mac[0] = (mac[0] & 0xFC) | 0x02` (LA set, unicast).
  Never a real OUI; never copied/observed.
- **Non-targeted:** probe requests only — no association, no data, no specific target.
- Persona-tuned (default from target): Ward (C5) ~8 phones, 2.4+5 GHz; Shade (C6) ~4, 2.4 GHz only.
- Files < 500 lines. Default build = BLE decoy (`SIMULACRA_PROBE`/`SIMULACRA_OBSERVE`/`CHURN_SELFTEST`
  all 0).

### Reusable recipes (C5 / ESP-IDF 5.5)

> **One-time retarget** (workspace is currently on esp32c6): run BUILD-RECIPE-C5 with
> `Remove-Item build,sdkconfig -Recurse -Force; idf.py set-target esp32c5` before the first build.

**BUILD-RECIPE-C5** (PowerShell):
```powershell
$py='C:\Program Files\Python312'; $env:PATH="$py;$py\Scripts;$env:PATH"
$env:IDF_PATH='~\esp\v5.5\esp-idf'
. ($env:IDF_PATH+'\export.ps1') | Out-Null
Set-Location '~\Downloads\simulacra'
idf.py build
```
**FLASH-RECIPE-C5:** append `idf.py -p COM12 flash`.
**SELFTEST-READ-C5:** `& '~\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe' '~\AppData\Local\Temp\simulacra_read_c5.py'` (resets + reads 18s; filter `SELFTEST`).
Self-test gated by `#define CHURN_SELFTEST 1` (Task 1); probe mode by `#define SIMULACRA_PROBE 1` (Task 2).

---

### Task 1: Probe-request frame builder + randomized MAC + self-test

**Files:** Create `main/probe.h`, `main/probe.c`; modify `main/CMakeLists.txt`,
`main/churn_selftest.c`.

- [ ] **Step 1: Create `main/probe.h`**
```c
#pragma once
#include <stdint.h>
#include <stddef.h>

#define PROBE_FRAME_MAX 64   // header(24) + wildcard SSID + rates + ext rates + DS param < 64

// Fill a randomized locally-administered, unicast MAC (Wi-Fi analog of BLE random-static).
void   probe_random_mac(uint8_t out[6]);
// Build a broadcast (wildcard-SSID) probe request for source `mac` on `channel`.
// Writes the 802.11 frame to out (<= PROBE_FRAME_MAX) and its length. Returns 0 on success.
int    probe_build_request(const uint8_t mac[6], uint8_t channel, uint8_t *out, size_t *out_len);

// Start Wi-Fi raw TX + the injection loop (Wi-Fi-only mode; BLE idle). Implemented in Task 2.
void   probe_start(void);
```

- [ ] **Step 2: Create `main/probe.c`** (frame logic only for now)
```c
#include <string.h>
#include "probe.h"
#include "esp_random.h"

void probe_random_mac(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[0] = (uint8_t)((out[0] & 0xFC) | 0x02);   // locally-administered, unicast
        int zero = 1, ff = 1;
        for (int i = 0; i < 6; i++) { if (out[i]) zero = 0; if (out[i] != 0xff) ff = 0; }
        if (!zero && !ff) return;
    }
}

int probe_build_request(const uint8_t mac[6], uint8_t channel, uint8_t *out, size_t *out_len)
{
    static const uint8_t hdr_tail[] = {
        0x00, 0x00,                         // SSID IE: id 0, len 0  (WILDCARD -- Law 3)
        0x01, 0x04, 0x02, 0x04, 0x0b, 0x16, // Supported Rates: 1,2,5.5,11 Mbps
        0x32, 0x04, 0x0c, 0x12, 0x18, 0x24, // Extended Supported Rates: 6,9,12,18 Mbps
        0x03, 0x01, 0x00,                   // DS Parameter Set: id 3, len 1, channel (filled below)
    };
    uint8_t *p = out;
    *p++ = 0x40; *p++ = 0x00;               // frame control: mgmt/probe-req, no flags
    *p++ = 0x00; *p++ = 0x00;               // duration
    memset(p, 0xff, 6); p += 6;             // DA broadcast
    memcpy(p, mac, 6); p += 6;              // SA = our randomized MAC
    memset(p, 0xff, 6); p += 6;             // BSSID broadcast
    *p++ = 0x00; *p++ = 0x00;               // seq control (driver overwrites when en_sys_seq=true)
    memcpy(p, hdr_tail, sizeof(hdr_tail)); p += sizeof(hdr_tail);
    out[p - out - 1] = channel;             // DS param channel = last byte
    *out_len = (size_t)(p - out);
    return 0;
}
```

- [ ] **Step 3: Register `probe.c`** — `main/CMakeLists.txt` SRCS: append `"probe.c"`.
(REQUIRES `esp_wifi esp_netif esp_event` is added in Task 2 when the radio code lands.)

- [ ] **Step 4: Self-test** — in `churn_selftest.c` add `#include "probe.h"` and:
```c
static void test_probe_frame(void)
{
    const uint8_t mac[6] = {0x12,0x34,0x56,0x78,0x9a,0xbc};
    uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
    ST_CHECK(probe_build_request(mac, 6, f, &n) == 0 && n >= 24 && n <= PROBE_FRAME_MAX, "probe frame builds, valid length");
    ST_CHECK(f[0] == 0x40 && f[1] == 0x00, "frame control = probe request");
    bool da=true, bssid=true, sa=true;
    for (int i=0;i<6;i++){ if (f[4+i]!=0xff) da=false; if (f[16+i]!=0xff) bssid=false; if (f[10+i]!=mac[i]) sa=false; }
    ST_CHECK(da && bssid, "DA + BSSID are broadcast");
    ST_CHECK(sa, "SA = supplied MAC");
    // body starts at offset 24: SSID IE must be wildcard (id 0, len 0)
    ST_CHECK(f[24]==0x00 && f[25]==0x00, "SSID IE is wildcard (len 0) -- Law 3");
    ST_CHECK(f[26]==0x01, "Supported Rates IE present");
    // Law-3 guard: no SSID IE (id 0) with non-zero length anywhere in the body
    bool directed=false;
    for (size_t i=24; i+1 < n; ) { uint8_t id=f[i], ln=f[i+1]; if (id==0x00 && ln!=0) directed=true; i += 2+ln; }
    ST_CHECK(!directed, "no directed (named-SSID) probe ever emitted");

    uint8_t a[6], b[6]; probe_random_mac(a); probe_random_mac(b);
    ST_CHECK((a[0]&0x03)==0x02, "random MAC is locally-administered + unicast");
    ST_CHECK(memcmp(a,b,6)!=0, "random MAC varies across calls");
}
```
Call `test_probe_frame();` in `churn_selftest_run()`.

- [ ] **Step 5: RED→GREEN.** Retarget to esp32c5 (one-time, see recipes). `CHURN_SELFTEST 1`.
BUILD-RECIPE-C5 (RED = link error before probe.c) → GREEN: FLASH + SELFTEST-READ-C5 → `PASS (fails=0)`.

- [ ] **Step 6: Commit**
```bash
git -C "~/Downloads/simulacra" add main/probe.h main/probe.c main/CMakeLists.txt main/churn_selftest.c
git -C "~/Downloads/simulacra" commit -m "feat(m7): probe-request frame builder + randomized LA MAC + self-test"
```

---

### Task 2: Wi-Fi raw TX + injection loop + SIMULACRA_PROBE mode

**Files:** Modify `main/probe.c` (radio path), `main/CMakeLists.txt` (REQUIRES), `main/simulacra_main.c`
(`SIMULACRA_PROBE` flag + app_main branch).

- [ ] **Step 1: Add the radio path to `main/probe.c`.** Includes + the injector:
```c
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "probe";

#if CONFIG_IDF_TARGET_ESP32C5
#define PROBE_PHONES   8
#define PROBE_USE_5G   1
#else
#define PROBE_PHONES   4
#define PROBE_USE_5G   0
#endif
#define PROBE_MAX_PHONES 12
#define PROBE_BURST_MS   2000
#define PROBE_ROTATE_MS  30000

static const uint8_t CH_24[] = { 1, 6, 11 };
#if PROBE_USE_5G
static const uint8_t CH_5[]  = { 36, 40, 44, 48, 149, 153, 157, 161 };
#endif

static uint8_t s_macs[PROBE_MAX_PHONES][6];
static int     s_n;
static int     s_hop;

static uint8_t next_channel(void)
{
    // interleave 2.4 GHz with 5 GHz (C5) so both bands get traffic
    s_hop++;
#if PROBE_USE_5G
    if (s_hop & 1) return CH_5[(s_hop/2) % (sizeof(CH_5)/sizeof(CH_5[0]))];
#endif
    return CH_24[(s_hop/ (PROBE_USE_5G?2:1)) % (sizeof(CH_24)/sizeof(CH_24[0]))];
}

static void probe_task(void *arg)
{
    s_n = PROBE_PHONES; if (s_n > PROBE_MAX_PHONES) s_n = PROBE_MAX_PHONES;
    for (int i = 0; i < s_n; i++) probe_random_mac(s_macs[i]);
    uint32_t last_rotate = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t ch = next_channel();
        int crc = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        int rc = 0;
        for (int i = 0; i < s_n; i++) {
            uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
            probe_build_request(s_macs[i], ch, f, &n);
            rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, true);
        }
        if (now - last_rotate >= PROBE_ROTATE_MS) {   // retire one fake phone -> fresh MAC
            last_rotate = now;
            probe_random_mac(s_macs[esp_random() % s_n]);
        }
        ESP_LOGW(TAG, "burst ch=%u phones=%d set_ch_rc=%d tx_rc=%d", ch, s_n, crc, rc);
        vTaskDelay(pdMS_TO_TICKS(PROBE_BURST_MS));
    }
}

void probe_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#if CONFIG_IDF_TARGET_ESP32C5
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);   // 2.4 + 5 GHz
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(probe_task, "probe", 4096, NULL, 5, NULL);
    ESP_LOGW(TAG, "probe injector started (phones=%d 5g=%d)", PROBE_PHONES, PROBE_USE_5G);
}
```

- [ ] **Step 2: REQUIRES** — `main/CMakeLists.txt`: `REQUIRES bt nvs_flash driver esp_wifi esp_netif esp_event`.

- [ ] **Step 3: Wire `SIMULACRA_PROBE` in `simulacra_main.c`.** Add the flag near the others:
```c
#ifndef SIMULACRA_PROBE
#define SIMULACRA_PROBE 0
#endif
```
and `#include "probe.h"`. In `app_main`, branch BEFORE the NimBLE setup (Wi-Fi-only mode skips BLE):
```c
    ESP_ERROR_CHECK(err);    // (after nvs_flash_init, existing)
#if SIMULACRA_PROBE
    probe_start();           // Wi-Fi-only: init Wi-Fi + injection task; NimBLE not started
    return;
#endif
```
(Leave the existing NimBLE init + `simulacra_task` for the BLE modes below it.)

- [ ] **Step 4: Build + flash + liveness.** Set `#define SIMULACRA_PROBE 1`, `CHURN_SELFTEST 0`.
BUILD-RECIPE-C5, FLASH-RECIPE-C5. SELFTEST-READ-C5 (or the long reader): expect
`probe injector started ...` then repeating `burst ch=.. phones=8 set_ch_rc=0 tx_rc=0` across changing
channels (including 5 GHz channel numbers on the C5). `tx_rc=0` = frames injected.

- [ ] **Step 5: Commit**
```bash
git -C "~/Downloads/simulacra" add main/probe.c main/CMakeLists.txt main/simulacra_main.c
git -C "~/Downloads/simulacra" commit -m "feat(m7): Wi-Fi raw-TX injector + SIMULACRA_PROBE mode (dual-band on C5)"
```

---

### Task 3: Hardware acceptance (sniffer) + docs + ship decoy

**Files:** Modify `main/simulacra_main.c` (`SIMULACRA_PROBE` back to 0), `README.md`.

- [ ] **Step 1: User-side sniffer acceptance.** With `SIMULACRA_PROBE` flashed on the C5, the user
runs a Wi-Fi sniffer (Wireshark monitor mode / Kismet / phone probe app). Confirm: **randomized-MAC
broadcast probe requests** at a plausible rate, **wildcard SSID** (no named SSIDs), appearing on
**both 2.4 and 5 GHz** channels. The serial `tx_rc=0` already confirmed injection from the dev side.
(If 5 GHz `set_ch_rc` is nonzero, note it — may need `esp_wifi_set_band(WIFI_BAND_5G)` before the
5 GHz `set_channel`; adjust `next_channel`/probe_task accordingly and re-verify.)

- [ ] **Step 2: Ship decoy.** Set `#define SIMULACRA_PROBE 0`. BUILD-RECIPE-C5, FLASH-RECIPE-C5 →
back to the BLE decoy. (Quick read: `decoy alive active=..`, no `rc=` errors.)

- [ ] **Step 3: Docs.** `README.md` — add a "Wi-Fi probe injection (M7)" note under Configuration:
build with `SIMULACRA_PROBE=1` for a Wi-Fi-only mode that emits synthetic, randomized-MAC **broadcast**
probe requests (never directed/named-SSID — the Wi-Fi analog of refined Law 3) at a phone-like rate,
channel-hopping 2.4 GHz (+5 GHz on the C5/Ward). Synthetic for now; Wi-Fi observe→match is a later
milestone, and BLE+Wi-Fi coexistence is M8.

- [ ] **Step 4: Commit**
```bash
git -C "~/Downloads/simulacra" add main/simulacra_main.c README.md
git -C "~/Downloads/simulacra" commit -m "feat(m7): ship decoy default; Wi-Fi probe-injection docs + C5 acceptance"
```

---

## Acceptance (M7, from the spec)

- A Wi-Fi sniffer sees randomized-MAC probe requests at a plausible rate. ✅ Task 3 Step 1.
- Broadcast/wildcard SSID only; never directed. ✅ self-test guard (Task 1) + sniffer (Task 3).
- Randomized locally-administered MACs; non-targeted. ✅ self-test + frame builder.
- Dual-band on the C5 (2.4 + 5 GHz). ✅ Task 2/3 (`set_ch_rc=0` on 5 GHz + sniffer on both bands).
- Default build still the BLE decoy. ✅ Task 3 Step 2.
```
