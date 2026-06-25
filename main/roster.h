#pragma once
#include <stddef.h>
#include <stdint.h>
#include "identity.h"

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
