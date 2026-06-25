#pragma once
#include <stddef.h>
#include <stdint.h>
#include "identity.h"
#include "churn_adv.h"   // CHURN_HW_INSTANCES

// Active-set / cooldown / time-slice tunables. ACTIVE_SET is the synthetic crowd
// size (how many identities are "present" at once); it is deliberately a plain
// constant so the population density is a one-line tunable (see Task 6).
#define CHURN_ACTIVE_SET        8
#define CHURN_TICK_MS           250
#define CHURN_SLICE_MS          1000
#define CHURN_DWELL_MIN_MS      180000u    // 3 min
#define CHURN_DWELL_MAX_MS      600000u    // 10 min
#define CHURN_COOLDOWN_MIN_MS   1800000u   // 30 min
#define CHURN_COOLDOWN_MAX_MS   3600000u   // 60 min

// apply(instance, id): place identity `id` on hardware `instance`. Return value
// (the adapter's rc) is ignored by the engine. Matches churn_adv_apply's int
// signature so the production adapter can be registered directly.
typedef int (*churn_apply_fn)(uint8_t instance, const identity_t *id);

void   churn_set_apply(churn_apply_fn fn);
void   churn_init(uint32_t now_ms);
void   churn_tick(uint32_t now_ms);
size_t churn_active_count(void);                 // non-NULL active slots
const identity_t *churn_active_at(size_t slot);  // may be NULL
