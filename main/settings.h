#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SIM_TARGET_FLOOR 4   // clamp floor: crowd never shrinks below this (STEALTH density)

typedef enum {
    SIM_PRESET_PAUSE = 0, SIM_PRESET_STEALTH, SIM_PRESET_NORMAL,
    SIM_PRESET_DENSE, SIM_PRESET_MAX, SIM_PRESET_COUNT
} sim_preset_t;

typedef struct {
    uint8_t  active_target;                       // concurrent phantom crowd size
    bool     paused;                              // freeze rotation (phantoms stay on-air)
    float    accel;                               // dwell shortening multiplier (>=1.0)
    uint32_t dwell_min_ms, dwell_max_ms;          // per-identity on-air window
    uint32_t cooldown_min_ms, cooldown_max_ms;    // retirement cooldown window
} sim_settings_t;

// Pure: resolve preset p to concrete settings against this board's `ceiling` (CHURN_ACTIVE_SET),
// already clamped. Returns 0 on success, -1 for an unknown preset. No side effects.
int  sim_settings_resolve(sim_preset_t p, uint8_t ceiling, sim_settings_t *out);

// Clamp settings to safe floors/ceilings in place (idempotent). Used on every apply so a forged
// or malformed command can never cross safe bounds.
void sim_settings_clamp(sim_settings_t *s, uint8_t ceiling);

// Apply settings to the churn engine now (no persistence).
void sim_settings_apply(const sim_settings_t *s);
// Resolve preset against CHURN_ACTIVE_SET, clamp, apply, and persist. 0 ok, -1 unknown preset.
int  sim_settings_apply_preset(sim_preset_t p);
// Load persisted settings from NVS (or firmware defaults) and apply. Call once at boot.
void sim_settings_init(void);
// Snapshot the current in-RAM settings.
void sim_settings_get(sim_settings_t *out);
// Web-UI granular path: clamp, apply, and persist an explicit settings struct.
void sim_settings_set(const sim_settings_t *s);
// Current pause state (convenience for the web UI toggle; avoids exposing the struct).
bool sim_settings_get_paused(void);
