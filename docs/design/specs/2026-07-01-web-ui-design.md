# Web UI — design (MVP)

> On-demand SoftAP config/telemetry web UI for the Simulacra ESP32-C5/C6 anti-tracking
> decoy. Local-only MVP; the security model is deliberately minimal and slated for a
> follow-up pass once a screen/credential-display path exists.

## Goal

Give a screenless Simulacra device a browser-based dashboard for viewing live decoy
telemetry (BLE churn, Wi-Fi probe activity, observed-population model, M9 Threat Radar)
and issuing a few basic controls — without adding a persistent, trackable RF beacon that
would undermine the device's own anti-tracking purpose.

## Locked decisions

- **Access model:** on-demand SoftAP via a **boot-window** trigger. On power-on the AP
  comes up for ~2 minutes; connect within the window or it auto-tears-down and the device
  goes full-decoy. Re-entering config mode requires a power-cycle (MVP).
- **Credentials:** **open AP + captive portal**, no password. Chosen to get it working;
  the SSID is fixed+documented, BSSID randomization and auth are deferred to the security
  pass (blocked on solving credential display without a screen).
- **UI scope:** read-only dashboard **plus basic controls** (toggle detection, toggle
  churn, clear threats, reboot).
- **Coexistence:** BLE churn keeps running during the config window; Wi-Fi probe injection
  pauses while the SoftAP owns the Wi-Fi interface.
- **Distribution:** committed **locally, not pushed** to GitHub for now.

## Approach

`esp_http_server` (IDF built-in) + a small UDP DNS responder for the captive portal,
serving a **single self-contained HTML page** embedded in flash via CMake
`EMBED_TXTFILES`. The page polls JSON endpoints every ~2 s and renders status cards +
control buttons.

- No filesystem partition (SPIFFS/LittleFS), no external assets, no WebSocket — smallest
  flash/RAM footprint and fastest path to a working MVP.
- Rejected alternatives: SPIFFS/LittleFS-hosted SPA (needs a partition + build tooling,
  overkill for one page); WebSocket live-push (needless complexity — fetch polling is
  sufficient for a human-paced dashboard).

## Modules & boundaries

### `main/webui.{h,c}` (new)
Self-contained: SoftAP bring-up/teardown, captive-portal DNS task, HTTP server
registration, request handlers, and status JSON serialization. Public surface:

```c
// Bring up the open SoftAP + captive DNS + HTTP server, serve until the timeout
// elapses or the page POSTs a "done" action, then tear everything down. Blocking.
void webui_run_config_window(uint32_t timeout_ms);

// Pure: serialize the current status snapshot into buf. Returns bytes written
// (excluding NUL) or negative on truncation. Unit-testable without radio/HTTP.
int  webui_build_status_json(char *buf, size_t len, const webui_status_t *st);
```

`webui` is a **read-only consumer** of the rest of the firmware: it gathers a
`webui_status_t` snapshot via existing public accessors plus a few new small getters, and
never reaches into another module's internals.

### `main/webui_index.html` (new)
The embedded page: status cards (decoy state, uptime, active fake-device count, churn
rate, probes sent, population summary, Threat Radar list), control buttons, and the
fetch/poll JS. Kept small (target well under ~20 KB) since it lives in flash as a blob.

## Lifecycle / coexistence (coordinator-sequenced)

The decoy task in `simulacra_main.c` (`#else` branch), gated on a new **source** flag
`SIMULACRA_WEBUI` (compile-time `#define`, like `SIMULACRA_DETECT`):

1. `coexist_start()` gains a `coexist_set_wifi_enabled(bool)` hook. Start it
   **Wi-Fi-disabled**: BLE churn, re-profiling, and M9 detection tick immediately, but
   `probe_wifi_init()` (STA) is deferred and Wi-Fi probe bursts are skipped.
2. `webui_run_config_window(120000)` brings up the open SoftAP (`WIFI_MODE_AP`,
   gateway 192.168.4.1) + DNS + HTTP. BLE is churning throughout the window.
3. On timeout or an explicit "done" from the page: `webui` tears down HTTP + DNS + AP,
   then the coordinator calls `coexist_set_wifi_enabled(true)`. Wi-Fi transitions cleanly
   **AP → STA** (no mode overlap), `probe_wifi_init()` runs, and probe injection resumes.
   Full decoy from here until the next power-cycle.

Rationale: `coexist.c` keeps owning radio orchestration; `webui` is a self-contained
AP+HTTP unit; the coordinator sequences the two so Wi-Fi ownership never overlaps between
AP and STA modes.

## HTTP endpoints

- `GET /` → the embedded page.
- Captive-portal probe URLs (`/generate_204`, `/gen_204`, `/hotspot-detect.html`,
  `/ncsi.txt`, `/connecttest.txt`, and unknown hosts via the DNS wildcard) → HTTP redirect
  to `/`, so the OS "sign-in" sheet auto-opens the dashboard.
- `GET /api/status` → JSON snapshot:
  - decoy state (churning / paused), uptime seconds, Wi-Fi state (AP-config / STA-decoy),
    current location-epoch;
  - active fake-device count, churn rate, cumulative Wi-Fi probes sent;
  - observed-population summary — **aggregates only** from the M5/M6 `rf_model`
    (population EWMA, total observations, active target);
  - Threat Radar — the M9 confirmed-follower list: **hash (not raw MAC)**, vendor, best
    RSSI, credited epochs, first/last epoch. Preserves Law-3 data discipline (the M9
    hash-only exception stands; no live MACs are exposed over the web).
- `POST /api/control` → `{ "action": "..." }`:
  - `detect_toggle` → `detect_set_enabled(!detect_enabled())` (both exist).
  - `churn_toggle` → new churn pause/resume flag consumed by the coexist tick.
  - `clear_threats` → new `detect_clear_threats()` (clears the RAM threat table + wipes the
    persisted threat blob in NVS namespace `"splinter"`).
  - `reboot` → `esp_restart()`.
  - `done` (optional) → signal `webui_run_config_window` to tear down early.

### New small functions required
- `coexist_set_wifi_enabled(bool)` — defer/resume the Wi-Fi (STA) side of the decoy.
- churn pause/resume flag (e.g. `churn_set_paused(bool)` / `churn_paused()`).
- `detect_clear_threats(void)` — clear RAM threats + wipe persisted threats.
- Getters for anything the status snapshot needs that isn't already public (roster/active
  count, probe counter, churn rate, rf_model live summary). `churn_active_count()`,
  `detect_threat_count()`, `detect_threat_at()`, `rf_model_load_nvs()` already exist.

## Testing

- **Pure-logic unit tests** added to the existing on-target self-test harness (the
  `churn_selftest.c` pattern, radio idle, PASS/FAIL over serial): construct a
  `webui_status_t`, call `webui_build_status_json`, and assert the presence, types, and
  bounds of the emitted fields, plus truncation behavior on an undersized buffer.
- **Hardware integration** (manual): flash, power-cycle, confirm the open AP appears, a
  phone's captive sheet pops the dashboard, live values update, each control acts, and the
  AP tears down after the window with the full decoy (incl. Wi-Fi probes) resuming.

## Known limitations (MVP — documented, deferred to the security pass)

- The AP is **open** during the window: anyone in RF range can connect and hit the control
  endpoints. Mitigated only by the short window, physical proximity, and local-only actions.
- **Fixed SSID**; no BSSID randomization yet — a (briefly-aired) stable identifier.
- Re-entering config mode requires a **power-cycle**; the ~2 min window is a fixed constant.
- No transport security (plain HTTP) and no CSRF protection on `POST /api/control`.

All acceptable for local single-user use. The security pass (blocked on a screenless
credential-display path — button-hold trigger, per-install-salt-derived password, BSSID
randomization, auth on controls) is tracked separately and gated on the hardware/UX
decision noted in the handoff.

## Conventions

- New source under `main/`; design doc here, implementation plan under
  `docs/design/plans/`. Commits `feat(webui):` / `fix(webui):` / `docs(webui):`, author
  Mummy-Dust, no AI-tooling references.
- Compile-time gate `SIMULACRA_WEBUI` (source `#define`, IDF `-D` sets CMake cache vars not
  C macros).
- Works on both personas — 2.4 GHz SoftAP for phone compatibility; no Ward (C5) / Shade
  (C6) divergence for the MVP.
- **Committed locally, not pushed.**
