#pragma once
#include <stddef.h>
#include <stdint.h>
#include "identity.h"
#include "rf_model.h"

// Persistent pool of pre-generated synthetic identities. Identities are created
// once at init (stable MAC + frozen payload) and cycled through the lifecycle
// IDLE -> ACTIVE -> COOLDOWN -> IDLE by churn.c. A retired identity reappears
// later with the SAME MAC/payload, so a scanner sees a device "come back" rather
// than a brand-new stranger every time.
#define CHURN_ROSTER_SIZE 256

void        roster_init(void);
identity_t *roster_promote_candidate(uint32_t now_ms);  // eligible identity (now IDLE) or NULL
size_t      roster_count_in_state(id_state_t s);
identity_t *roster_at(size_t i);                        // for tests
void        make_random_static_addr_pub(uint8_t out[6]);// always static-random (top bits 11)
void        make_random_addr(uint8_t out[6], uint8_t top2);   // random addr with given top-2-bits
void        make_random_addr_mixed(uint8_t out[6]);    // random addr, realistic static/RPA/NRPA mix
identity_t *roster_pick_company(uint16_t company_id);   // roster entry with this company (0=anonymous), or NULL
// M8 live re-profiling: regenerate ONLY the IDLE identities from a fresh model. ACTIVE and
// COOLDOWN identities keep their MAC/payload, so the visible crowd turns over gradually
// (fresh room-matched identities phase in as churn promotes them; no hard swap).
void        roster_reseed_idle(const rf_model_t *m);
