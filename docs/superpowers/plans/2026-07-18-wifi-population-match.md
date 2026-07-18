# Wi-Fi Population-Match Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive the probe-agent count from observed ambient phone density (a passive promiscuous count of randomized-MAC probe sources) instead of the fixed `PROBE_PHONES`, closing the Wi-Fi density tell.

**Architecture:** A pure host-testable estimator `main/wifi_density.{c,h}` (hashed-MAC rolling table → density → EWMA target); a thin firmware glue `main/wifi_observe.{c,h}` (promiscuous RX on the STA interface) that feeds it; `probe_agents_set_target` makes the agent count runtime-adjustable; `coexist.c` starts the observe and updates the target on its re-profile tick. Mirrors the `probe_agents.c` (pure) / `probe.c` (glue) split.

**Tech Stack:** C (ESP-IDF firmware; MSVC `cl` for host tests), Python 3.12 stdlib `unittest`. No new deps.

## Global Constraints

- **Law 1 (aggregates only):** observed real MACs are hashed-and-dropped (salted FNV-1a, exactly like `observe.c`); the estimator stores only a hash + timestamp, never a raw MAC.
- **Law 4 (population-match):** target = `clamp(round(EWMA density), WIFI_OBS_FLOOR, WIFI_OBS_CAP)`; `WIFI_OBS_FLOOR` default **2**, `WIFI_OBS_CAP` = **16** (= `PROBE_AGENTS_MAX`), `WIFI_OBS_TTL_MS` default **180000** (3 min), `WIFI_OBS_FALLBACK` default **6**.
- `wifi_density.c` is **pure** (no ESP headers) so `probe_dump` compiles it; `wifi_observe.c` is firmware-only glue (esp_wifi).
- Single board never counts its own decoys (`esp32-no-self-rx`); fleetmate decoys excluded via `fleet_mac_excluded`; only **randomized (locally-administered)** probe sources counted.
- Host tests from `tools/probe_audit/` via `"C:/Program Files/Python312/python.exe" -m pytest -q`; `cl` build from a "Developer PowerShell for VS". Firmware compile-verify **esp32c6** (`idf.py -B build_c6 build`).
- **Never hardcode the OS username / forbidden committer name into a tracked file** (`private/TOOLING-GOTCHAS.md`) — scan for absolute path prefixes only.
- Commit identity `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>`; every commit ends with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.
- Branch `feat/wifi-population-match`; merge to `main` `--no-ff`, PII-scan the range, push at the end.

---

## File Structure

- **Create** `main/wifi_density.{h,c}` — pure estimator (host-testable).
- **Create** `main/wifi_observe.{h,c}` — promiscuous RX glue (firmware only).
- **Modify** `main/probe_agents.{h,c}` — `probe_agents_set_target`.
- **Modify** `main/coexist.c` — one-time `wifi_obs_start`, per-reprofile target update.
- **Modify** `main/CMakeLists.txt` — SRCS + forward `WIFI_OBS_*` flags.
- **Modify** `tools/probe_audit/probe_dump.c` — `--wifiobs` (stdin-driven) and `--settarget` host harness modes.
- **Modify** `tools/probe_audit/{Makefile,run.ps1}` — add `wifi_density.c` to the `probe_dump` build.
- **Create** `tools/probe_audit/tests/test_wifi_density.py`, `tools/probe_audit/tests/test_probe_set_target.py`.

---

### Task 1: `wifi_density` pure estimator + host harness

**Files:**
- Create: `main/wifi_density.h`, `main/wifi_density.c`
- Modify: `tools/probe_audit/probe_dump.c` (add `--wifiobs` near the top of `main()`)
- Modify: `tools/probe_audit/Makefile`, `tools/probe_audit/run.ps1`
- Test: `tools/probe_audit/tests/test_wifi_density.py`

**Interfaces:**
- Produces: `wifi_obs_reset(uint32_t salt)`, `wifi_obs_note(const uint8_t mac[6], uint32_t now_ms)`, `int wifi_obs_density(uint32_t now_ms)`, `int wifi_obs_target(uint32_t now_ms)`. Task 3 (glue) consumes all four.

- [ ] **Step 1: Write the failing test**

Create `tools/probe_audit/tests/test_wifi_density.py`:

```python
import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def wifiobs(script):
    out = subprocess.run([EXE, "--wifiobs"], input=script, capture_output=True, text=True).stdout
    return [int(x) for x in out.split()]


def _macs(n):
    return "".join("note 02000000%04x 0\n" % i for i in range(1, n + 1))


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class WifiDensity(unittest.TestCase):
    def test_distinct_count(self):
        self.assertEqual(wifiobs("reset 1\n" + _macs(3) + "density 0\n"), [3])

    def test_dedup_same_mac(self):
        self.assertEqual(wifiobs("reset 1\nnote 020000000001 0\nnote 020000000001 100\ndensity 100\n"), [1])

    def test_ttl_expiry(self):
        # WIFI_OBS_TTL_MS default 180000 -> a MAC last seen at 0 is gone by 200000
        self.assertEqual(wifiobs("reset 1\nnote 020000000001 0\ndensity 200000\n"), [0])

    def test_target_floor_when_empty(self):
        self.assertEqual(wifiobs("reset 1\n" + "target 0\n" * 20)[-1], 2)     # WIFI_OBS_FLOOR

    def test_target_tracks_density(self):
        self.assertEqual(wifiobs("reset 1\n" + _macs(10) + "target 0\n" * 20)[-1], 10)

    def test_target_caps_at_max(self):
        self.assertEqual(wifiobs("reset 1\n" + _macs(24) + "target 0\n" * 20)[-1], 16)   # WIFI_OBS_CAP


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_wifi_density.py -q`
Expected: FAIL — `--wifiobs` mode does not exist, `wifiobs()` returns `[]`, index errors / assertion fails.

- [ ] **Step 3: Create `main/wifi_density.h`**

```c
#pragma once
#include <stdint.h>

// Wi-Fi ambient phone-density estimator (PURE; host-testable; no radio). A rolling table of HASHED
// probe-request source MACs (raw MAC never stored -- Law 1) with a TTL; the live distinct count is a
// recently-active real-phone proxy that drives the probe-agent population target (Law 4).

#ifndef WIFI_OBS_TTL_MS
#define WIFI_OBS_TTL_MS 180000u   // forget a MAC not re-heard within ~3 min
#endif
#ifndef WIFI_OBS_FLOOR
#define WIFI_OBS_FLOOR 2          // min fake phones even in a near-empty room (a couple read as plausible)
#endif
#ifndef WIFI_OBS_CAP
#define WIFI_OBS_CAP 16           // = PROBE_AGENTS_MAX; agent ceiling
#endif

void wifi_obs_reset(uint32_t salt);
// Hash `mac` (salted FNV-1a; raw MAC consumed here, never stored) and add/refresh it in the rolling
// table (evict oldest when full). Call only for real (non-fleet, randomized) probe sources.
void wifi_obs_note(const uint8_t mac[6], uint32_t now_ms);
// Distinct non-expired hashed MACs = recently-active real-phone proxy.
int  wifi_obs_density(uint32_t now_ms);
// Density mapped + EWMA-smoothed to a probe-agent target in [WIFI_OBS_FLOOR, WIFI_OBS_CAP].
int  wifi_obs_target(uint32_t now_ms);
```

- [ ] **Step 4: Create `main/wifi_density.c`**

```c
#include "wifi_density.h"
#include <string.h>

#define OBS_CAP 64
typedef struct { uint32_t hash; uint32_t last_ms; int used; } obs_e;

static obs_e    s_tbl[OBS_CAP];
static uint32_t s_salt;
static int      s_ewma_x16;   // EWMA of density, fixed-point (value << 4)

void wifi_obs_reset(uint32_t salt)
{
    memset(s_tbl, 0, sizeof s_tbl);
    s_salt = salt;
    s_ewma_x16 = 0;
}

static uint32_t hash_mac(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_salt;             // FNV-1a offset basis, salted
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

void wifi_obs_note(const uint8_t mac[6], uint32_t now_ms)
{
    uint32_t h = hash_mac(mac);                    // raw MAC consumed here; only the hash is kept
    int slot = -1, oldest_i = 0; uint32_t oldest = 0;
    for (int i = 0; i < OBS_CAP; i++) {
        if (s_tbl[i].used && s_tbl[i].hash == h) { s_tbl[i].last_ms = now_ms; return; }   // refresh
        if (!s_tbl[i].used) { if (slot < 0) slot = i; continue; }
        uint32_t age = now_ms - s_tbl[i].last_ms;
        if (age >= WIFI_OBS_TTL_MS && slot < 0) slot = i;                                 // reuse expired
        if (age > oldest) { oldest = age; oldest_i = i; }
    }
    if (slot < 0) slot = oldest_i;                 // full of live entries: evict the oldest
    s_tbl[slot].used = 1; s_tbl[slot].hash = h; s_tbl[slot].last_ms = now_ms;
}

int wifi_obs_density(uint32_t now_ms)
{
    int n = 0;
    for (int i = 0; i < OBS_CAP; i++)
        if (s_tbl[i].used && (uint32_t)(now_ms - s_tbl[i].last_ms) < WIFI_OBS_TTL_MS) n++;
    return n;
}

int wifi_obs_target(uint32_t now_ms)
{
    int d = wifi_obs_density(now_ms);
    s_ewma_x16 += ((d << 4) - s_ewma_x16) / 4;     // EWMA alpha=1/4 (fixed-point)
    int t = (s_ewma_x16 + 8) >> 4;                 // round to nearest
    if (t < WIFI_OBS_FLOOR) t = WIFI_OBS_FLOOR;
    if (t > WIFI_OBS_CAP)   t = WIFI_OBS_CAP;
    return t;
}
```

- [ ] **Step 5: Add the `--wifiobs` harness mode to `tools/probe_audit/probe_dump.c`**

Add the include near the top (with the other `main/` includes):

```c
#include "wifi_density.h"
```

Insert this block at the very start of `main()` (before the first existing `if (argc > 1 ...)`):

```c
    if (argc > 1 && strcmp(argv[1], "--wifiobs") == 0) {
        char line[64], cmd[16], mh[16];
        unsigned u;
        while (fgets(line, sizeof line, stdin)) {
            if (sscanf(line, "%15s", cmd) != 1) continue;
            if (strcmp(cmd, "reset") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                wifi_obs_reset(u);
            } else if (strcmp(cmd, "note") == 0 && sscanf(line, "%*s %12s %u", mh, &u) == 2) {
                uint8_t m[6];
                for (int i = 0; i < 6; i++) { char b[3] = { mh[2 * i], mh[2 * i + 1], 0 }; m[i] = (uint8_t)strtoul(b, 0, 16); }
                wifi_obs_note(m, u);
            } else if (strcmp(cmd, "density") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                printf("%d\n", wifi_obs_density(u));
            } else if (strcmp(cmd, "target") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                printf("%d\n", wifi_obs_target(u));
            }
        }
        return 0;
    }
```

- [ ] **Step 6: Add `wifi_density.c` to the `probe_dump` build**

In `tools/probe_audit/run.ps1`, append `..\..\main\wifi_density.c` to the `cl` source list (the line ending in `host_stubs\ble_devices_stub.c`):

```powershell
           probe_dump.c ..\..\main\probe_frame.c ..\..\main\probe_agents.c ..\..\main\uniq_id.c ..\..\main\phantom.c host_stubs\ble_devices_stub.c ..\..\main\wifi_density.c /Fe:probe_dump.exe | Out-Null
```

In `tools/probe_audit/Makefile`, append `$(ROOT)/main/wifi_density.c` to the `SRC` list (mirror however the existing sources are listed).

- [ ] **Step 7: Rebuild `probe_dump.exe`**

From a Developer PowerShell for VS, in `tools/probe_audit`:

```powershell
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main probe_dump.c ..\..\main\probe_frame.c ..\..\main\probe_agents.c ..\..\main\uniq_id.c ..\..\main\phantom.c host_stubs\ble_devices_stub.c ..\..\main\wifi_density.c /Fe:probe_dump.exe
```

Expected: compiles clean; `probe_dump.exe` regenerated.

- [ ] **Step 8: Run the test to verify it passes**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_wifi_density.py -q`
Expected: PASS (6 tests).

- [ ] **Step 9: Commit**

```bash
git add main/wifi_density.h main/wifi_density.c tools/probe_audit/probe_dump.c \
        tools/probe_audit/run.ps1 tools/probe_audit/Makefile tools/probe_audit/tests/test_wifi_density.py
git commit -F - <<'EOF'
feat(wifi-popmatch): pure Wi-Fi density estimator + host harness

wifi_density: hashed-MAC rolling table (Law-1 hash-and-drop), TTL distinct
count, EWMA -> clamped agent target. Host-tested via probe_dump --wifiobs.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 2: runtime-adjustable agent target

**Files:**
- Modify: `main/probe_agents.h`, `main/probe_agents.c`
- Modify: `tools/probe_audit/probe_dump.c` (add `--settarget`)
- Test: `tools/probe_audit/tests/test_probe_set_target.py`

**Interfaces:**
- Consumes: existing `probe_agents_init`, `probe_agents_count`, and the file-static `agent_spawn`, `s_agents`, `s_n`.
- Produces: `void probe_agents_set_target(int n, uint32_t now_ms)`. Task 3 (coexist) consumes it.

- [ ] **Step 1: Write the failing test**

Create `tools/probe_audit/tests/test_probe_set_target.py`:

```python
import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def settarget(seq):
    out = subprocess.check_output([EXE, "--settarget", "1", "8"] + [str(x) for x in seq], text=True)
    return [int(x) for x in out.split()]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SetTarget(unittest.TestCase):
    def test_grow_shrink_clamp(self):
        # init 8 -> [8]; then set 4, 16, 0(->1), 99(->16)
        self.assertEqual(settarget([4, 16, 0, 99]), [8, 4, 16, 1, 16])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_probe_set_target.py -q`
Expected: FAIL — `--settarget` mode does not exist.

- [ ] **Step 3: Declare `probe_agents_set_target` in `main/probe_agents.h`**

Add after `probe_agents_init` (line ~26):

```c
// Adjust the live agent set to n (clamped to [1, PROBE_AGENTS_MAX]): spawn to grow, drop to shrink.
// The population-match knob (mirrors churn_set_active_target on the BLE side).
void     probe_agents_set_target(int n, uint32_t now_ms);
```

- [ ] **Step 4: Implement in `main/probe_agents.c`**

Add after `probe_agents_init` (which ends at line ~37):

```c
void probe_agents_set_target(int n, uint32_t now_ms)
{
    if (n < 1) n = 1;
    if (n > PROBE_AGENTS_MAX) n = PROBE_AGENTS_MAX;
    for (int i = s_n; i < n; i++) agent_spawn(&s_agents[i], now_ms);   // grow: spawn the new slots
    s_n = n;                                                           // shrink: higher slots go dormant
}
```

- [ ] **Step 5: Add the `--settarget` harness mode to `tools/probe_audit/probe_dump.c`**

Insert at the start of `main()` (near `--wifiobs`):

```c
    if (argc > 1 && strcmp(argv[1], "--settarget") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n0   = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 8;
        srand(seed);
        probe_agents_init(n0, 0);
        printf("%d\n", probe_agents_count());
        for (int i = 4; i < argc; i++) {
            probe_agents_set_target((int)strtol(argv[i], 0, 10), (uint32_t)(i * 1000));
            printf("%d\n", probe_agents_count());
        }
        return 0;
    }
```

- [ ] **Step 6: Rebuild `probe_dump.exe`**

Rerun the `cl` command from Task 1 Step 7 (probe_agents.c is already in the source list). Expected: clean build.

- [ ] **Step 7: Run the test to verify it passes**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_probe_set_target.py -q`
Expected: PASS. Then the whole suite: `"C:/Program Files/Python312/python.exe" -m pytest -q` → all pass (was 35; now ~42).

- [ ] **Step 8: Commit**

```bash
git add main/probe_agents.h main/probe_agents.c tools/probe_audit/probe_dump.c \
        tools/probe_audit/tests/test_probe_set_target.py
git commit -F - <<'EOF'
feat(wifi-popmatch): runtime-adjustable probe agent target

probe_agents_set_target(n) grows (spawn) / shrinks (dormant) the live agent
set, clamped to [1, PROBE_AGENTS_MAX] -- the Wi-Fi population-match knob.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 3: promiscuous observe glue + coexist wiring

**Files:**
- Create: `main/wifi_observe.h`, `main/wifi_observe.c`
- Modify: `main/coexist.c` (the task loop ~line 244, and `coexist_reprofile` ~line 199)
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `wifi_obs_reset/note/density/target` (Task 1); `probe_agents_set_target` (Task 2); `fleet_mac_excluded` (`fleet.h`).
- Produces: `bool wifi_obs_start(void)`; `WIFI_OBS_FALLBACK`.

**Verification note:** this task is firmware glue (esp_wifi + coexist) — not host-compiled. It is verified by **compile-verify (esp32c6)**; the promiscuous-coexistence and density-tracking behavior is **HW-gated** (recorded, not merge-blocking — the host cores in Tasks 1-2 gate the merge).

- [ ] **Step 1: Create `main/wifi_observe.h`**

```c
#pragma once
#include <stdbool.h>

// Firmware glue: enable promiscuous RX on the STA interface and feed randomized probe-request
// source MACs into wifi_density (raw MAC hashed-and-dropped there). Our own frames are never
// received (esp32 has no self-RX); fleetmate decoys are excluded via fleet_mac_excluded.

#ifndef WIFI_OBS_FALLBACK
#define WIFI_OBS_FALLBACK 6   // conservative fixed agent target when promiscuous can't start (< PROBE_PHONES)
#endif

// Enable promiscuous probe observe. Returns true iff promiscuous enabled (else caller uses the fallback).
bool wifi_obs_start(void);
```

- [ ] **Step 2: Create `main/wifi_observe.c`**

```c
#include "wifi_observe.h"
#include "wifi_density.h"
#include "fleet.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "wifiobs";

static void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    if (p->rx_ctrl.sig_len < 24) return;
    const uint8_t *f = p->payload;
    if (f[0] != 0x40) return;                 // frame control: probe request
    const uint8_t *sa = f + 10;               // source MAC
    if (!(sa[0] & 0x02)) return;              // randomized (locally-administered) only = real-phone proxy
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (fleet_mac_excluded(sa, now)) return;  // skip fleetmate decoys (our own are never received)
    wifi_obs_note(sa, now);                   // raw MAC hashed-and-dropped inside
}

bool wifi_obs_start(void)
{
    wifi_obs_reset((uint32_t)esp_random());
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    if (esp_wifi_set_promiscuous_filter(&filt) != ESP_OK) { ESP_LOGW(TAG, "filter set failed -> fallback"); return false; }
    if (esp_wifi_set_promiscuous_rx_cb(rx_cb)  != ESP_OK) { ESP_LOGW(TAG, "rx cb set failed -> fallback");  return false; }
    if (esp_wifi_set_promiscuous(true)         != ESP_OK) { ESP_LOGW(TAG, "promiscuous enable failed -> fallback"); return false; }
    ESP_LOGW(TAG, "wifi observe up (promiscuous on STA)");
    return true;
}
```

- [ ] **Step 3: Wire the one-time start into `main/coexist.c`'s task loop**

Add the includes near the other `main/` includes at the top of `coexist.c`:

```c
#include "wifi_observe.h"
#include "wifi_density.h"
#include "probe_agents.h"
```

Add a file-static near the other coexist statics:

```c
static bool s_wifi_obs_started = false;
static bool s_wifi_obs_ok = false;
```

In `coexist_task` (inside the `for(;;)` loop, right after `if (d.fire_wifi && s_wifi_ok) { ... }` block, i.e. after line ~259), add the one-time start once Wi-Fi is confirmed up:

```c
        if (s_wifi_ok && !s_wifi_obs_started) {
            s_wifi_obs_ok = wifi_obs_start();     // enable promiscuous once the STA/injection side is up
            s_wifi_obs_started = true;
        }
```

- [ ] **Step 4: Update the target on the re-profile tick in `coexist_reprofile`**

At the end of `coexist_reprofile` (the function at ~line 199, after it sets the BLE `active_target`), add the Wi-Fi population-match update:

```c
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int wifi_target = s_wifi_obs_ok ? wifi_obs_target(now) : WIFI_OBS_FALLBACK;
    probe_agents_set_target(wifi_target, now);
    ESP_LOGW(TAG, "wifi popmatch: density=%d -> agents=%d%s",
             s_wifi_obs_ok ? wifi_obs_density(now) : -1, wifi_target,
             s_wifi_obs_ok ? "" : " (fallback)");
```

(If `coexist_reprofile` already computes a `now`, reuse it instead of re-declaring.)

- [ ] **Step 5: Add sources + flags to `main/CMakeLists.txt`**

Append `"wifi_density.c" "wifi_observe.c"` to the `SRCS` list. Append the flags to the `foreach(flag ...)` list:

```cmake
    OBSERVE_LOG_RSSI SIMULACRA_FLEET_SIZE WIFI_OBS_FLOOR WIFI_OBS_CAP WIFI_OBS_TTL_MS WIFI_OBS_FALLBACK)
```

- [ ] **Step 6: Compile-verify (esp32c6)**

Activate ESP-IDF, then from the repo root:

```powershell
. C:\Users\<idf>\esp\v5.5\esp-idf\export.ps1     # use the machine's IDF export (do NOT hardcode a username in any committed file)
idf.py -B build_c6 build
```

Expected: build succeeds (`wifi_density.c` + `wifi_observe.c` compile into the firmware; `esp32c6` image generated). Source is target-independent, so c5 compiles identically (a c5 build needs a slow `set-target`).

- [ ] **Step 7: On-target validation (hardware; record, not merge-blocking)**

Flash a C5/C6 decoy; confirm: no boot crash; injection continues; the `wifi popmatch: density=.. -> agents=..` log appears and `density` tracks the number of real phones nearby (bring a phone in/out). Then capture ~30 min with Kismet and re-run `tools/probe_audit/probe_behavior_scorecard.py` on it — confirm `density_dominance` drops from 1.0. Record the before/after in `private/KISMET-VALIDATION.md`. If hardware is unavailable at implementation time, note it deferred — the Task 1-2 host tests + compile-verify gate the merge.

- [ ] **Step 8: Commit**

```bash
git add main/wifi_observe.h main/wifi_observe.c main/coexist.c main/CMakeLists.txt
git commit -F - <<'EOF'
feat(wifi-popmatch): promiscuous observe glue + coexist population-match

wifi_observe enables promiscuous RX on the STA iface and feeds randomized
probe sources (excl. fleet peers; own frames never received) into wifi_density.
coexist starts it once and, each re-profile tick, sets the probe-agent target
from observed density (or WIFI_OBS_FALLBACK if promiscuous is unavailable).
Compile-verified esp32c6; promiscuous+coexist behavior is HW-gated.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

## Finishing

After Tasks 1-2 host tests pass and Task 3 compiles, use **superpowers:finishing-a-development-branch**:
- Verify the full `tools/probe_audit` suite is green; merge `feat/wifi-population-match` to `main` `--no-ff`.
- PII-scan the merged range (`C:\Users`, `/Users/`, `/home/`, the OS username, real MACs) before pushing.
- Push `main`.
- Update `private/PROJECT-MAP.md` §11 (Wi-Fi population-match done; HW validation pending), `private/KISMET-VALIDATION.md`, and memory (the density fix exists; audit measures it).

## Self-Review

**Spec coverage:** wifi_observe pure core (hash-and-drop, TTL, density, EWMA target) → Task 1; promiscuous glue (probe-req filter, randomized-only, fleet exclusion, `wifi_obs_start` returns status) → Task 3; `probe_agents_set_target` → Task 2; coexist start + per-reprofile target with fallback → Task 3; CMakeLists SRCS + flags → Task 3; floor=2/cap=16/ttl=180000/fallback=6 → Global Constraints + code. All spec sections map to a task. (Plan realizes the spec's single "wifi_observe module" as a pure `wifi_density.c` + glue `wifi_observe.c` split, so the estimator is host-testable — same pure/glue idiom as `probe_agents.c`/`probe.c`.)

**Placeholder scan:** no TBD/TODO; every code step is complete. Task 3 Step 6's `<idf>` is a deliberate instruction to NOT hardcode the username in a committed file, not a code placeholder.

**Type consistency:** `wifi_obs_reset/note/density/target` signatures identical across Task 1 (def), the `--wifiobs` harness (Task 1), and the glue (Task 3). `probe_agents_set_target(int, uint32_t)` identical across Task 2 def, `--settarget` harness, and Task 3 coexist call. `WIFI_OBS_FLOOR`(2)/`WIFI_OBS_CAP`(16)/`WIFI_OBS_TTL_MS`(180000) in `wifi_density.h`; `WIFI_OBS_FALLBACK`(6) in `wifi_observe.h`; all forwarded in CMakeLists (Task 3). The `--wifiobs` MAC format (12 hex chars) matches the test's `"02000000%04x"` (6 bytes).
