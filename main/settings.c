#include "settings.h"
#include "churn.h"    // CHURN_DWELL_*/CHURN_COOLDOWN_* firmware defaults

static uint32_t u32_clamp(uint32_t v, uint32_t lo, uint32_t hi)
{ return v < lo ? lo : (v > hi ? hi : v); }

void sim_settings_clamp(sim_settings_t *s, uint8_t ceiling)
{
    if (ceiling < SIM_TARGET_FLOOR) ceiling = SIM_TARGET_FLOOR;
    if (s->active_target < SIM_TARGET_FLOOR) s->active_target = SIM_TARGET_FLOOR;
    if (s->active_target > ceiling) s->active_target = ceiling;
    if (s->accel < 1.0f) s->accel = 1.0f;
    if (s->accel > 4.0f) s->accel = 4.0f;
    s->dwell_min_ms = u32_clamp(s->dwell_min_ms, 30000u, 900000u);
    s->dwell_max_ms = u32_clamp(s->dwell_max_ms, s->dwell_min_ms, 900000u);
    s->cooldown_min_ms = u32_clamp(s->cooldown_min_ms, 300000u, 3600000u);
    s->cooldown_max_ms = u32_clamp(s->cooldown_max_ms, s->cooldown_min_ms, 3600000u);
}

int sim_settings_resolve(sim_preset_t p, uint8_t ceiling, sim_settings_t *out)
{
    if (p >= SIM_PRESET_COUNT) return -1;
    uint8_t stealth = (uint8_t)((ceiling * 4) / 10);   // ~40% of ceiling
    sim_settings_t s = {
        .active_target = ceiling, .paused = false, .accel = 1.0f,
        .dwell_min_ms = CHURN_DWELL_MIN_MS, .dwell_max_ms = CHURN_DWELL_MAX_MS,
        .cooldown_min_ms = CHURN_COOLDOWN_MIN_MS, .cooldown_max_ms = CHURN_COOLDOWN_MAX_MS,
    };
    switch (p) {
    case SIM_PRESET_PAUSE:                                  // NORMAL values, rotation frozen
        s.paused = true; break;
    case SIM_PRESET_STEALTH:
        s.active_target = stealth; s.dwell_min_ms = 300000; s.dwell_max_ms = 600000; break;
    case SIM_PRESET_NORMAL:
        break;                                              // firmware defaults
    case SIM_PRESET_DENSE:
        s.accel = 1.5f; s.dwell_min_ms = 90000; s.dwell_max_ms = 240000;
        s.cooldown_min_ms = 900000; s.cooldown_max_ms = 1800000; break;
    case SIM_PRESET_MAX:
        s.accel = 2.5f; s.dwell_min_ms = 45000; s.dwell_max_ms = 120000;
        s.cooldown_min_ms = 600000; s.cooldown_max_ms = 1200000; break;
    default: return -1;
    }
    sim_settings_clamp(&s, ceiling);
    *out = s;
    return 0;
}
