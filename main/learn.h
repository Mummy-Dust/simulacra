#pragma once
#include "sdkconfig.h"       // CONFIG_IDF_TARGET_ESP32C5 (persona-sized cap)
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "templates.h"       // fmt_family_t
#include "learn_record.h"    // learned_template_t + learn_shape_hash (shared component)

// --- store sizing / lifecycle tunables ---
#if CONFIG_IDF_TARGET_ESP32C5
#define LEARN_CAP 128
#else
#define LEARN_CAP 64
#endif
#ifndef LEARN_AGEOUT_SWEEPS
#define LEARN_AGEOUT_SWEEPS 720   // ~ hours at a 15 s sweep; tunable
#endif
#ifndef LEARN_MIN_SIGHTINGS
#define LEARN_MIN_SIGHTINGS 3
#endif
#ifndef LEARN_CAND_SLOTS
#define LEARN_CAND_SLOTS 24
#endif

#define LEARN_DB_MAGIC   0x4C524E31u   // "LRN1"
#define LEARN_DB_VERSION 1

// Parse a clean AD into a skeleton. false => rejected (forbidden / unparseable /
// over budget). Fills skeleton, rand_mask, name region, company_id, svc_uuid,
// family; leaves interval / shape_hash / counters zero.
bool learn_strip(const uint8_t *ad, uint8_t len, uint16_t company,
                 learned_template_t *out);

// (learn_shape_hash is declared in learn_record.h — shared with the Vigil librarian.)

// Copy skeleton, rewrite rand_mask bytes with esp_random(), overwrite the name
// region with a synthetic name, sample an interval in [itvl_min,itvl_max].
// Always Law-3-clean, <=31 B. Returns 0 on success.
int learn_render(const learned_template_t *t, uint8_t out[31],
                 uint8_t *out_len, uint16_t *out_itvl_ms);

// --- store ---
void   learn_reset(void);                       // clear store + candidates (RAM)
size_t learn_count(void);
const learned_template_t *learn_at(size_t i);
bool   learn_store_add(const learned_template_t *t, uint16_t sweep_no);
void   learn_age_out(uint16_t sweep_no);

// --- candidate pipeline (observe hook) ---
void learn_offer(uint32_t mac_hash, const uint8_t *ad, uint8_t len,
                 uint16_t company, uint32_t now_ms);
void learn_end_sweep(uint16_t sweep_no);

// --- NVS persistence (namespace "splinter", key "learn_db") ---
int learn_save_nvs(void);
int learn_load_nvs(void);
