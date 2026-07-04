#pragma once
#include <stdint.h>
#include "identity.h"
#include "rf_model.h"

#define GEN_MIN_OBS 50   // below this total_obs, the model is too sparse -> caller falls back

// Diversity floor target: when an observed company exceeds this % of the model, generation
// redirects a proportional fraction of its draws to the varied built-in template families, so a
// model skewed toward one loud vendor (a room full of Galaxy Buds) still yields a mixed crowd.
// Below this share a vendor is left alone (full ambient match); above it, it's pulled back toward it.
#ifndef GEN_MAX_VENDOR_PCT
#define GEN_MAX_VENDOR_PCT 40
#endif

// Fill `roster[0..n)` by sampling the model: vendor weighted by observed mix, payload from the
// matching template or a generic vendor-mfg, interval from that vendor's histogram, fresh
// random-static MAC, dithered TX. Every identity gets a valid archetype_idx and a non-empty,
// Law-3-clean payload. Returns the number of identities successfully built (== n on success).
size_t  generate_roster(const rf_model_t *m, identity_t *roster, size_t n);

// Population-matched active-set target from pop_ewma, persona-tuned and clamped.
uint8_t generate_active_target(const rf_model_t *m);

// Log the generated roster's company-id histogram over serial (acceptance/inspection).
void    generate_dump_roster(const identity_t *roster, size_t n);
