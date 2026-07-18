#include "wifi_density.h"
#include <string.h>

#define OBS_CAP 64
typedef struct { uint32_t hash; uint32_t last_ms; int used; } obs_e;

static obs_e    s_tbl[OBS_CAP];
static uint32_t s_salt;
static int      s_ewma_x16;   // EWMA of density, fixed-point (value << 4)

void wifi_obs_reset(uint32_t salt)
{
    memset(s_tbl, 0, sizeof s_tbl);
    s_salt = salt;
    s_ewma_x16 = 0;
}

static uint32_t hash_mac(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_salt;             // FNV-1a offset basis, salted
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

void wifi_obs_note(const uint8_t mac[6], uint32_t now_ms)
{
    uint32_t h = hash_mac(mac);                    // raw MAC consumed here; only the hash is kept
    int slot = -1, oldest_i = 0; uint32_t oldest = 0;
    for (int i = 0; i < OBS_CAP; i++) {
        if (s_tbl[i].used && s_tbl[i].hash == h) { s_tbl[i].last_ms = now_ms; return; }   // refresh
        if (!s_tbl[i].used) { if (slot < 0) slot = i; continue; }
        uint32_t age = now_ms - s_tbl[i].last_ms;
        if (age >= WIFI_OBS_TTL_MS && slot < 0) slot = i;                                 // reuse expired
        if (age > oldest) { oldest = age; oldest_i = i; }
    }
    if (slot < 0) slot = oldest_i;                 // full of live entries: evict the oldest
    s_tbl[slot].used = 1; s_tbl[slot].hash = h; s_tbl[slot].last_ms = now_ms;
}

int wifi_obs_density(uint32_t now_ms)
{
    int n = 0;
    for (int i = 0; i < OBS_CAP; i++)
        if (s_tbl[i].used && (uint32_t)(now_ms - s_tbl[i].last_ms) < WIFI_OBS_TTL_MS) n++;
    return n;
}

int wifi_obs_target(uint32_t now_ms)
{
    int d = wifi_obs_density(now_ms);
    s_ewma_x16 += ((d << 4) - s_ewma_x16) / 4;     // EWMA alpha=1/4 (fixed-point)
    int t = (s_ewma_x16 + 8) >> 4;                 // round to nearest
    if (t < WIFI_OBS_FLOOR) t = WIFI_OBS_FLOOR;
    if (t > WIFI_OBS_CAP)   t = WIFI_OBS_CAP;
    return t;
}
