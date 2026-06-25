#pragma once
#include <stdint.h>
#include <stddef.h>
#include "rf_model.h"

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
