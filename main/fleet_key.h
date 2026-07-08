#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FLEET_NVS_NS "simfleet"

// Load-or-create the NVS X25519 identity keypair; load the fleet key/epoch if present.
void fleet_key_init(void);
// True once a usable fleet transport key is available. (Always true when the
// SIMULACRA_FLEET_PROVISION gate is off — the compile-time key is the fallback.)
bool fleet_key_have(void);
// Current 32-byte fleet transport key. NULL only when provisioning is enabled and
// no key has been enrolled yet. When the gate is off, returns SIMULACRA_ESPNOW_KEY.
const uint8_t *fleet_key_get(void);
// Previous 32-byte key retained across a rotation for the grace window, or NULL.
const uint8_t *fleet_key_prev(void);
// Store a new fleet key + epoch (retiring the current key to prev); persists to NVS.
void fleet_key_set(const uint8_t key[32], uint32_t epoch);
uint32_t fleet_key_epoch(void);
// 32-byte X25519 identity public key (available after fleet_key_init).
const uint8_t *fleet_id_pk(void);
// Copy the 32-byte identity secret (enrollment responder only).
void fleet_id_sk(uint8_t out[32]);
// Format "xxxx-xxxx-xxxx-xxxx" from SHA-512(id_pk)[0..7]; cap must be >= 20.
void fleet_id_fingerprint(char *out, size_t cap);
