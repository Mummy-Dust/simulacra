#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "detect.h"   // detect_threat_t, DETECT_MAX_THREATS

// One immutable snapshot of decoy state, rendered to JSON for the dashboard.
// Aggregates + hash-only threats only (Law 3): no raw MAC ever appears here.
typedef struct {
    uint32_t uptime_s;
    bool     decoy_paused;       // churn rotation paused via the UI
    bool     wifi_config_mode;   // true while the config AP is up
    bool     tx_degraded;        // probe TX wedged (alive but not injecting) -> health flag bit2
    bool     battery_low;        // fuel-gauge SoC below threshold (early brownout warning) -> flag bit3
    uint16_t active_devices;     // churn_active_count()
    uint16_t roster_size;        // CHURN_ROSTER_SIZE
    uint32_t probes_sent;        // cumulative Wi-Fi probes injected
    uint16_t epoch;              // coexist_current_epoch()
    uint16_t pop_ewma;           // M5/M6 observed-population EWMA (aggregate)
    uint32_t total_obs;          // M5/M6 total observations (aggregate)
    uint8_t  active_target;      // churn active-target (population match)
    uint8_t  threat_count;       // valid entries in threats[]
    detect_threat_t threats[DETECT_MAX_THREATS];
    uint8_t  form_restless, form_wandering, form_bound;   // BLE shade-form counts: RPA/NRPA/static
} webui_status_t;

// Pure: render st into buf as JSON. Returns bytes written (excl. NUL), or -1 if
// the buffer was too small (buf left NUL-terminated at the truncation point).
int webui_build_status_json(char *buf, size_t len, const webui_status_t *st);

// Gather a live snapshot from the running decoy (defined in Task 3; radio-side).
void webui_gather_status(webui_status_t *out);

// Bring up the open SoftAP + captive DNS + HTTP, serve until timeout_ms elapses
// or the page POSTs "done", then tear it all down. Blocking. (Defined in Task 3.)
void webui_run_config_window(uint32_t timeout_ms);
