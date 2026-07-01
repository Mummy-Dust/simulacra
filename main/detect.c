#include <string.h>
#include "detect.h"

typedef struct {
    bool     used;
    uint32_t hash;
    uint16_t vendor;
    uint16_t cur_epoch;            // epoch of the current run of sightings
    uint8_t  sightings_this_epoch;
    bool     credited;             // already credited cur_epoch?
    uint8_t  distinct_epochs;      // credited distinct epochs so far
    int8_t   best_rssi;
    uint16_t first_epoch;
    uint16_t last_seen_epoch;
    uint32_t lru;                  // last-use tick (for LRU eviction, Task 2)
} candidate_t;

static candidate_t    s_cand[DETECT_MAX_CANDIDATES];
static detect_threat_t s_threat[DETECT_MAX_THREATS];
static size_t         s_threat_n;
static uint32_t       s_lru;
static bool           s_enabled = true;

void detect_reset(void)
{
    memset(s_cand, 0, sizeof(s_cand));
    memset(s_threat, 0, sizeof(s_threat));
    s_threat_n = 0;
    s_lru = 0;
    s_enabled = true;
}

void detect_set_enabled(bool en) { s_enabled = en; }
bool detect_enabled(void)        { return s_enabled; }

static detect_threat_t *threat_find(uint32_t hash)
{
    for (size_t i = 0; i < s_threat_n; i++) if (s_threat[i].hash == hash) return &s_threat[i];
    return NULL;
}

// Find an existing candidate, else allocate a fresh one (LRU-evict when full).
static candidate_t *cand_find_or_alloc(uint32_t hash, uint16_t epoch, int8_t rssi)
{
    candidate_t *victim = NULL;
    for (size_t i = 0; i < DETECT_MAX_CANDIDATES; i++) {
        if (s_cand[i].used && s_cand[i].hash == hash) return &s_cand[i];
        if (!s_cand[i].used && !victim) victim = &s_cand[i];   // first free slot
    }
    if (!victim) {   // no free slot: evict lowest-lru used slot
        victim = &s_cand[0];
        for (size_t i = 1; i < DETECT_MAX_CANDIDATES; i++)
            if (s_cand[i].lru < victim->lru) victim = &s_cand[i];
    }
    memset(victim, 0, sizeof(*victim));
    victim->used = true; victim->hash = hash;
    victim->cur_epoch = epoch; victim->first_epoch = epoch;
    victim->best_rssi = rssi;
    return victim;
}

static detect_threat_t *promote(candidate_t *c)
{
    detect_threat_t *t = threat_find(c->hash);
    if (!t) {
        if (s_threat_n < DETECT_MAX_THREATS) t = &s_threat[s_threat_n++];
        else {   // full: overwrite the least-recently-seen threat
            t = &s_threat[0];
            for (size_t i = 1; i < s_threat_n; i++)
                if (s_threat[i].last_epoch < t->last_epoch) t = &s_threat[i];
        }
    }
    t->hash = c->hash; t->vendor = c->vendor; t->epochs = c->distinct_epochs;
    t->best_rssi = c->best_rssi; t->first_epoch = c->first_epoch; t->last_epoch = c->cur_epoch;
    c->used = false;   // candidate graduates to a threat
    return t;
}

detect_result_t detect_observe(uint32_t hash, int8_t rssi, uint16_t vendor, uint16_t epoch)
{
    if (!s_enabled) return DETECT_NONE;

    detect_threat_t *t = threat_find(hash);
    if (t) {
        if (rssi > t->best_rssi) t->best_rssi = rssi;
        t->last_epoch = epoch;
        return DETECT_KNOWN;
    }

    candidate_t *c = cand_find_or_alloc(hash, epoch, rssi);
    c->vendor = vendor;
    c->last_seen_epoch = epoch;
    c->lru = ++s_lru;
    if (rssi > c->best_rssi) c->best_rssi = rssi;

    if (epoch != c->cur_epoch) {          // rolled into a new epoch
        c->cur_epoch = epoch;
        c->sightings_this_epoch = 0;
        c->credited = false;
    }
    if (c->sightings_this_epoch < 255) c->sightings_this_epoch++;

    if (!c->credited && c->sightings_this_epoch >= DETECT_MIN_SIGHTINGS) {
        c->credited = true;
        c->distinct_epochs++;
        if (c->distinct_epochs >= DETECT_EPOCH_STRIKES) { promote(c); return DETECT_CONFIRM; }
    }
    return DETECT_NONE;
}

void detect_on_epoch_change(uint16_t epoch) { (void)epoch; /* aging added in Task 2 */ }

size_t detect_threat_count(void) { return s_threat_n; }

bool detect_threat_at(size_t i, detect_threat_t *out)
{
    if (i >= s_threat_n || !out) return false;
    *out = s_threat[i];
    return true;
}
