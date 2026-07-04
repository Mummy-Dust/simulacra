#pragma once
#include <stdint.h>
#include <stddef.h>
#include "rf_model.h"
#include "threat_sig.h"     // sig_hit_t (fingerprint match passed through the report tap)

// --- capture-side primitives (pure; unit-tested without the radio) ---

// Wipe the ephemeral dedup table and set the per-boot hashing salt (never persisted).
void     observe_reset_ephemeral(uint32_t boot_salt);
// Salted FNV-1a over the 6-byte MAC. The MAC is read here and never stored.
uint32_t observe_hash_mac(const uint8_t mac[6]);
// Ingest one feature-extracted observation: hash the MAC for dedup, estimate the interval
// from the last sighting, and fold features into the model. The raw MAC is dropped.
void     observe_ingest(rf_model_t *m, const uint8_t mac[6], uint32_t now_ms,
                        uint16_t company_id, int8_t rssi, uint8_t pdu_type);
// Close the current sweep window: fold distinct-device count + arrivals into the model
// (EWMA) and wipe the ephemeral table.
void     observe_end_sweep(rf_model_t *m, uint32_t window_ms);
// Distinct hashes currently held in the ephemeral table (for tests/heartbeat).
size_t   observe_ephemeral_count(void);

// --- live radio path (implemented in Task 4) ---
void     observe_start(uint32_t boot_salt);   // load model from NVS, start passive scan
// Periodic liveness line (scan rc + running totals); call from the idle observe loop so the
// device is observable even when ambient BLE traffic is sparse.
void     observe_heartbeat(void);

// --- live re-profiling (M8): bounded observe window while advertising continues ---
// Load the persistent model once (call before the first observe_window).
void              observe_reprofile_init(uint32_t boot_salt);
// Run ONE bounded scan window (blocks the caller for duration_ms), ingesting reports while
// ext-adv keeps running, then close the sweep and fold into the model.
void              observe_window(uint32_t duration_ms);
// The current persistent model (RAM).
const rf_model_t *observe_model(void);

// --- report tap (M9): observe publishes each raw report before hashing it away ---
// `hit` is a fingerprint match for this advert (M10), or NULL if none matched.
typedef void (*observe_report_cb_t)(const uint8_t mac[6], int8_t rssi, uint16_t company_id,
                                    const sig_hit_t *hit);
// Register a callback fired for every scan report BEFORE the MAC is hashed/dropped. NULL
// disables. Observe stays decoupled from any consumer.
void observe_set_report_cb(observe_report_cb_t cb);
