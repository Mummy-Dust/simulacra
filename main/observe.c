#include <string.h>
#include "observe.h"

#define OBS_TABLE_CAP 256

typedef struct { uint32_t hash; uint32_t first_ms; uint32_t last_ms; bool used; } obs_entry_t;

static obs_entry_t s_tbl[OBS_TABLE_CAP];
static uint32_t    s_salt;
static uint32_t    s_arrivals;     // new distinct hashes this window
static bool        s_saturated;

void observe_reset_ephemeral(uint32_t boot_salt)
{
    memset(s_tbl, 0, sizeof(s_tbl));
    s_salt = boot_salt;
    s_arrivals = 0;
    s_saturated = false;
}

uint32_t observe_hash_mac(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_salt;     // FNV-1a offset basis, salted
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

void observe_ingest(rf_model_t *m, const uint8_t mac[6], uint32_t now_ms,
                    uint16_t company_id, int8_t rssi, uint8_t pdu_type)
{
    uint32_t h = observe_hash_mac(mac);    // MAC consumed here, never stored
    int32_t interval = -1;
    obs_entry_t *slot = NULL, *freep = NULL;
    for (size_t i = 0; i < OBS_TABLE_CAP; i++) {
        if (s_tbl[i].used && s_tbl[i].hash == h) { slot = &s_tbl[i]; break; }
        if (!s_tbl[i].used && !freep) freep = &s_tbl[i];
    }
    if (slot) {
        interval = (int32_t)(now_ms - slot->last_ms);
        slot->last_ms = now_ms;
    } else if (freep) {
        freep->used = true; freep->hash = h; freep->first_ms = now_ms; freep->last_ms = now_ms;
        s_arrivals++;
    } else {
        s_saturated = true;                // full: still counted in the model, just not deduped
    }
    rf_model_observe(m, company_id, rssi, pdu_type, interval);
}

void observe_end_sweep(rf_model_t *m, uint32_t window_ms)
{
    uint32_t distinct = 0;
    for (size_t i = 0; i < OBS_TABLE_CAP; i++) if (s_tbl[i].used) distinct++;
    rf_model_end_sweep(m, distinct, window_ms, s_arrivals);
    memset(s_tbl, 0, sizeof(s_tbl));       // wipe ephemeral identifiers
    s_arrivals = 0;
    s_saturated = false;
}

size_t observe_ephemeral_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < OBS_TABLE_CAP; i++) if (s_tbl[i].used) n++;
    return n;
}
