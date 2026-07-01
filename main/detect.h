#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- tunables (spec §3/§4) ---
#ifndef DETECT_EPOCH_STRIKES
#define DETECT_EPOCH_STRIKES     3      // distinct credited epochs -> confirmed follower
#endif
#ifndef DETECT_MIN_SIGHTINGS
#define DETECT_MIN_SIGHTINGS     2      // adv reports in an epoch to earn a presence-credit
#endif
#ifndef DETECT_AGE_OUT_EPOCHS
#define DETECT_AGE_OUT_EPOCHS    8      // drop an un-promoted candidate unseen this many epochs
#endif
#ifndef DETECT_MAX_CANDIDATES
#define DETECT_MAX_CANDIDATES    48     // RAM candidate table (LRU-evicted)
#endif
#ifndef DETECT_MAX_THREATS
#define DETECT_MAX_THREATS       8      // confirmed threats retained + persisted
#endif
#ifndef DETECT_LOCATE_RSSI_DELTA
#define DETECT_LOCATE_RSSI_DELTA 6      // dB change that emits a locate update
#endif
#ifndef DETECT_LOCATE_MIN_MS
#define DETECT_LOCATE_MIN_MS     10000  // periodic locate heartbeat interval
#endif
#ifndef DETECT_LOCATE_FLOOR_MS
#define DETECT_LOCATE_FLOOR_MS   1500   // hard rate cap: never emit a locate faster than this per threat
#endif

typedef enum { DETECT_NONE = 0, DETECT_CONFIRM, DETECT_KNOWN } detect_result_t;

// A confirmed follower (also the persisted record shape). Hash-only; no raw MAC.
typedef struct {
    uint32_t hash;
    uint16_t vendor;
    uint8_t  epochs;        // distinct credited epochs at/after confirmation (>= STRIKES)
    int8_t   best_rssi;
    uint16_t first_epoch;
    uint16_t last_epoch;
} detect_threat_t;

// --- pure core (no radio, no NVS in the decision path) ---
void            detect_reset(void);                 // clear all RAM state (tests + boot)
void            detect_set_enabled(bool en);
bool            detect_enabled(void);
detect_result_t detect_observe(uint32_t hash, int8_t rssi, uint16_t vendor, uint16_t epoch);
void            detect_on_epoch_change(uint16_t epoch);
// Locate-throttle (pure): should a `THREAT locate` line be emitted for this sighting?
bool            detect_locate_due(int8_t rssi, int8_t last_rssi, uint32_t now_ms, uint32_t last_ms);
// Self-exclusion (pure): is `mac` present in the given set of `n` 6-byte MACs?
bool            detect_mac_in_set(const uint8_t mac[6], const uint8_t set[][6], size_t n);

size_t          detect_threat_count(void);
bool            detect_threat_at(size_t i, detect_threat_t *out);

// --- persistence (NVS namespace "splinter"; called by the coordinator, not the decision path) ---
uint32_t        detect_load_salt(void);        // load-or-create the per-install detection salt
int             detect_save_nvs(void);         // persist confirmed threats; 0 = ok
int             detect_load_nvs(void);         // restore confirmed threats; 0 = ok
// Clear the RAM threat table AND wipe the persisted threats (keeps the salt).
void            detect_clear_threats(void);
// Drain the newly-confirmed-threat flag (coexist_task): true + *out once per confirmation.
bool            detect_drain_pending(detect_threat_t *out);
