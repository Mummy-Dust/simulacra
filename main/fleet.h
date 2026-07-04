#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Fleet self-exclusion: a rolling table of OTHER decoys' active synthetic MACs, learned over the
// ESP-NOW link, so a decoy doesn't model / learn / detect its fleet-mates' fake devices as if they
// were real. Pure + always-built (empty when standalone). Fed by esp_now_link (RADAR_TYPE_FLEET_MACS),
// queried by observe.

#ifndef FLEET_MAC_CAP
#define FLEET_MAC_CAP 48          // peer synthetic MACs tracked (~3 decoys x 16 active)
#endif
#ifndef FLEET_MAC_TTL_MS
#define FLEET_MAC_TTL_MS 90000u   // forget a peer MAC not re-heard within this (3x broadcast + margin)
#endif

#define RADAR_TYPE_FLEET_MACS 6   // decoy -> all: my current active synthetic MACs

void   fleet_reset(void);
// Add/refresh peer synthetic MACs heard over the fleet link. Reuses free/expired slots, else evicts
// the oldest.
void   fleet_note_peer_macs(const uint8_t (*macs)[6], size_t n, uint32_t now_ms);
// True iff `mac` is a known, non-expired fleet-peer MAC (caller skips detect/learn/model for it).
bool   fleet_mac_excluded(const uint8_t mac[6], uint32_t now_ms);
size_t fleet_peer_count(uint32_t now_ms);        // non-expired entries (tests/diag)

// Wire framing: [uint8 count][count * 6 MAC bytes]. Pure, unit-testable.
size_t fleet_macs_pack(uint8_t *out, size_t out_max, const uint8_t (*macs)[6], size_t n);
size_t fleet_macs_unpack(const uint8_t *in, size_t len, uint8_t (*macs)[6], size_t max);
