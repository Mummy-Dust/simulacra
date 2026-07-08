#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Vigil's fleet state: the current transport key + epoch and the decoy-identity
// allowlist, sealed (AES-256-GCM) on SD at /sdcard/simulacra/fleet.db. The sealing
// key is derived from Vigil's control secret (SIMULACRA_CTRL_SK), so an exfiltrated
// SD card alone does not reveal the fleet key.

#define FLEET_ALLOW_CAP 32

// Load fleet.db from SD (call after the card is mounted); generates a fresh random
// fleet key + empty allowlist on first run. Safe to call once at boot.
void fleet_db_load(void);
// Atomic sealed write (temp + rename). No-op if SD is unavailable.
void fleet_db_save(void);

const uint8_t *fleet_db_key(void);     // 32-byte current fleet transport key
uint32_t       fleet_db_epoch(void);
void           fleet_db_rotate(void);  // new random key, epoch++ (does not auto-save)

bool   fleet_allow_contains(const uint8_t id_pk[32]);
bool   fleet_allow_add(const uint8_t id_pk[32]);     // false if full; true if added or already present
bool   fleet_allow_remove(const uint8_t id_pk[32]);  // false if not found
size_t fleet_allow_count(void);
const uint8_t *fleet_allow_at(size_t i);             // 32-byte pubkey, or NULL if out of range

// Seal/open the in-RAM state to/from a self-contained blob (pure; used by load/save
// and the self-test). out must hold >= fleet_db_blob_cap() bytes.
size_t fleet_db_blob_cap(void);
int  fleet_db_seal_blob(uint8_t *out, size_t *out_len);
int  fleet_db_open_blob(const uint8_t *buf, size_t len);

// On-target self-check (seal round-trip + allowlist ops). Logs pass/fail; returns 0 on pass.
int fleet_db_selftest(void);
