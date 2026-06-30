#include <string.h>
#include "churn.h"
#include "roster.h"
#include "esp_random.h"

// Active set: the identities currently "present" in the synthetic crowd. There
// are more of them (CHURN_ACTIVE_SET) than hardware radios (CHURN_HW_INSTANCES),
// so churn_tick time-slices the active set across the radios. s_occupant tracks
// which identity is currently programmed on each radio to avoid redundant applies.
static identity_t   *s_active[CHURN_ACTIVE_SET];
static identity_t   *s_occupant[CHURN_HW_INSTANCES];
static uint32_t      s_phase;
static uint32_t      s_last_slice_ms;
static churn_apply_fn s_apply;
static uint8_t        s_active_target = CHURN_ACTIVE_SET;  // M6: runtime population-match size

void churn_set_active_target(uint8_t n)
{
    if (n < 1) n = 1;
    if (n > CHURN_ACTIVE_SET) n = CHURN_ACTIVE_SET;
    s_active_target = n;
}

static uint32_t rnd_range(uint32_t lo, uint32_t hi)
{
    return lo + (esp_random() % (hi - lo + 1));
}

void churn_set_apply(churn_apply_fn fn) { s_apply = fn; }

void churn_init(uint32_t now_ms)
{
    s_phase = 0; s_last_slice_ms = now_ms;
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) s_occupant[i] = NULL;
    for (int i = 0; i < CHURN_ACTIVE_SET; i++) s_active[i] = NULL;
    for (int i = 0; i < s_active_target; i++) {
        identity_t *id = roster_promote_candidate(now_ms);
        s_active[i] = id;
        if (id) {
            id->state = ID_ACTIVE;
            id->active_until_ms = now_ms + rnd_range(1, CHURN_DWELL_MAX_MS); // staggered seed
        }
    }
}

void churn_tick(uint32_t now_ms)
{
    for (int s = 0; s < s_active_target; s++) {
        identity_t *id = s_active[s];
        if (id && now_ms >= id->active_until_ms) {
            id->state = ID_COOLDOWN;
            id->eligible_at_ms = now_ms + rnd_range(CHURN_COOLDOWN_MIN_MS, CHURN_COOLDOWN_MAX_MS);
            identity_t *c = roster_promote_candidate(now_ms);
            s_active[s] = c;
            if (c) {
                c->state = ID_ACTIVE;
                c->active_until_ms = now_ms + rnd_range(CHURN_DWELL_MIN_MS, CHURN_DWELL_MAX_MS);
            }
        }
    }
    if (now_ms - s_last_slice_ms >= CHURN_SLICE_MS) {
        s_last_slice_ms = now_ms; s_phase++;
        for (int i = 0; i < CHURN_HW_INSTANCES; i++) {
            int idx;
            if (s_active_target <= CHURN_HW_INSTANCES) {
                if (i >= s_active_target) continue;      // fewer identities than radios
                idx = i;                                 // static mapping, 100% duty
            } else {
                idx = (s_phase * CHURN_HW_INSTANCES + i) % s_active_target;
            }
            identity_t *target = s_active[idx];
            if (target && target != s_occupant[i]) {
                s_occupant[i] = target;
                if (s_apply) s_apply((uint8_t)i, target);
            }
        }
    }
}

size_t churn_active_count(void)
{
    size_t n = 0;
    for (int i = 0; i < s_active_target; i++) if (s_active[i]) n++;
    return n;
}

const identity_t *churn_active_at(size_t slot)
{
    return (slot < s_active_target) ? s_active[slot] : NULL;
}
