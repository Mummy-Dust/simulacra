#include <string.h>
#include "detect.h"
#include "nvs.h"
#include "esp_random.h"

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
static bool           s_pending;
static detect_threat_t s_pending_threat;

void detect_reset(void)
{
    memset(s_cand, 0, sizeof(s_cand));
    memset(s_threat, 0, sizeof(s_threat));
    s_threat_n = 0;
    s_lru = 0;
    s_enabled = true;
    s_pending = false;
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
    s_pending = true; s_pending_threat = *t;   // hand off to the coordinator for NVS persist + LED
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

void detect_on_epoch_change(uint16_t epoch)
{
    for (size_t i = 0; i < DETECT_MAX_CANDIDATES; i++) {
        if (!s_cand[i].used) continue;
        if (s_cand[i].distinct_epochs >= DETECT_EPOCH_STRIKES) continue;  // (already promoted anyway)
        uint16_t gap = (uint16_t)(epoch - s_cand[i].last_seen_epoch);
        if (gap >= DETECT_AGE_OUT_EPOCHS) s_cand[i].used = false;         // forget the follower-less device
    }
}

bool detect_locate_due(int8_t rssi, int8_t last_rssi, uint32_t now_ms, uint32_t last_ms)
{
    uint32_t elapsed = (uint32_t)(now_ms - last_ms);
    if (elapsed >= DETECT_LOCATE_MIN_MS)   return true;    // periodic heartbeat
    if (elapsed <  DETECT_LOCATE_FLOOR_MS) return false;   // hard rate cap -> no per-packet spam
    int d = (int)rssi - (int)last_rssi;
    if (d < 0) d = -d;
    return d >= DETECT_LOCATE_RSSI_DELTA;                  // "getting warmer", but rate-capped
}

bool detect_mac_in_set(const uint8_t mac[6], const uint8_t set[][6], size_t n)
{
    for (size_t i = 0; i < n; i++) if (memcmp(mac, set[i], 6) == 0) return true;
    return false;
}

size_t detect_threat_count(void) { return s_threat_n; }

bool detect_threat_at(size_t i, detect_threat_t *out)
{
    if (i >= s_threat_n || !out) return false;
    *out = s_threat[i];
    return true;
}

// --- persistence: per-install salt + confirmed-threat blob (NVS namespace "splinter") ---
#define DETECT_NVS_NS    "splinter"       // reuse the existing namespace (do not rename)
#define DETECT_KEY_SALT  "detect_salt"
#define DETECT_KEY_THR   "detect_thr"
#define DETECT_THR_MAGIC 0x4D394454u       // 'M9DT'

bool detect_drain_pending(detect_threat_t *out)
{
    if (!s_pending) return false;
    if (out) *out = s_pending_threat;
    s_pending = false;
    return true;
}

uint32_t detect_load_salt(void)
{
    nvs_handle_t h; uint32_t salt = 0;
    if (nvs_open(DETECT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return esp_random() | 1u;
    if (nvs_get_u32(h, DETECT_KEY_SALT, &salt) != ESP_OK || salt == 0) {
        salt = esp_random() | 1u;                 // never zero (test asserts non-zero)
        nvs_set_u32(h, DETECT_KEY_SALT, salt);
        nvs_commit(h);
    }
    nvs_close(h);
    return salt;
}

typedef struct { uint32_t magic; uint16_t count; detect_threat_t thr[DETECT_MAX_THREATS]; } detect_blob_t;

int detect_save_nvs(void)
{
    detect_blob_t b; memset(&b, 0, sizeof(b));
    b.magic = DETECT_THR_MAGIC; b.count = (uint16_t)s_threat_n;
    for (size_t i = 0; i < s_threat_n; i++) b.thr[i] = s_threat[i];
    nvs_handle_t h;
    if (nvs_open(DETECT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, DETECT_KEY_THR, &b, sizeof(b));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (int)e;
}

int detect_load_nvs(void)
{
    detect_blob_t b; size_t len = sizeof(b);
    nvs_handle_t h;
    if (nvs_open(DETECT_NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_get_blob(h, DETECT_KEY_THR, &b, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(b) || b.magic != DETECT_THR_MAGIC) return -1;
    s_threat_n = (b.count <= DETECT_MAX_THREATS) ? b.count : DETECT_MAX_THREATS;
    for (size_t i = 0; i < s_threat_n; i++) s_threat[i] = b.thr[i];
    return 0;
}
