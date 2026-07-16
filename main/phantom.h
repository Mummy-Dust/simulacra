#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "probe_frame.h"     // probe_arch_t
#include "probe_agents.h"    // PROBE_AGENTS_MAX

// A cross-protocol persona: the shared lifecycle + device family that binds one BLE identity
// and one Wi-Fi probe identity into a single synthetic dual-radio "device". Named `phantom`
// (not `persona`) to avoid colliding with coexist_persona_t (the board coexistence profile).
typedef enum { PF_SAMSUNG, PF_GOOGLE, PF_APPLE, PF_GENERIC, PHANTOM_FAMILY_COUNT } phantom_family_t;

typedef struct {
    phantom_family_t family;
    uint32_t born_ms;
    uint32_t life_ms;
    uint32_t generation;     // bumped on each reincarnation; bound members re-sync on change
    bool     alive;
} phantom_t;

#define PHANTOM_MAX PROBE_AGENTS_MAX     // one persona per probe agent (bind from the Wi-Fi side)

void  phantom_init(int n, uint32_t now_ms);      // create n phantoms (clamped to PHANTOM_MAX)
int   phantom_lifecycle(uint32_t now_ms);        // retire+reincarnate expired; returns # reborn
int   phantom_count(void);
const phantom_t *phantom_at(int i);
probe_arch_t phantom_arch(phantom_family_t f);   // family -> Wi-Fi archetype
uint16_t     phantom_company(phantom_family_t f);// family -> BLE company id (0 = anonymous RPA)
