#pragma once
#include <stdint.h>
#include <stdbool.h>

// Layer-1 entry: init Wi-Fi under coexistence (BLE-only fallback on failure) and spawn
// the coordinator task that owns churn_tick + Wi-Fi bursts + re-profile. Call after the
// BLE roster/churn are initialized and the NimBLE host is synced.
void coexist_start(void);

// --- testable scheduler core (pure; no radio) ---
typedef struct {
    uint32_t wifi_period_ms;        // Wi-Fi burst cadence
    uint32_t reprofile_period_ms;   // live re-profile cadence
    bool     use_5g;                // C5/Ward: batch 5 GHz excursions
    float    drift_threshold;       // anti-entourage trigger (Ward effectively off)
} coexist_persona_t;

// The persona for this build target (Ward on C5, Shade on C6).
const coexist_persona_t *coexist_persona(void);

typedef struct { bool fire_wifi; bool fire_reprofile; } coexist_due_t;

// Decide what is due at now_ms given last-fire timestamps; advances each timestamp that
// fires. Pure (no radio); shared by the coordinator task and the self-test.
coexist_due_t coexist_due(const coexist_persona_t *p, uint32_t now_ms,
                          uint32_t *last_wifi_ms, uint32_t *last_reprofile_ms);
