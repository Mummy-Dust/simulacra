#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Fleet self-exclusion: a rolling table of OTHER decoys' active synthetic MACs, learned over the
// ESP-NOW link, so a decoy doesn't model / learn / detect its fleet-mates' fake devices as if they
// were real. Pure + always-built (empty when standalone). Fed by esp_now_link (RADAR_TYPE_FLEET_MACS),
// queried by observe.

#ifndef FLEET_MAC_CAP
#define FLEET_MAC_CAP 96          // peer synthetic MACs tracked (~2 peers x [16 BLE + 16 probe] + headroom)
#endif
#ifndef FLEET_BCAST_MACS_MAX
#define FLEET_BCAST_MACS_MAX 32   // max MACs packed per FLEET_MACS broadcast (sealed <=250: 32*6+1=193 + 32 = 225)
#endif
#ifndef FLEET_MAC_TTL_MS
#define FLEET_MAC_TTL_MS 90000u   // forget a peer MAC not re-heard within this (3x broadcast + margin)
#endif
#ifndef FLEET_NODE_CAP
#define FLEET_NODE_CAP 8          // distinct peer nodes tracked (real ESP-NOW hardware MACs, not synthetic)
#endif

#define RADAR_TYPE_FLEET_MACS 6   // decoy -> all: my current active synthetic MACs

void   fleet_reset(void);
// Add/refresh peer synthetic MACs heard over the fleet link. Reuses free/expired slots, else evicts
// the oldest.
void   fleet_note_peer_macs(const uint8_t (*macs)[6], size_t n, uint32_t now_ms);
// True iff `mac` is a known, non-expired fleet-peer MAC (caller skips detect/learn/model for it).
bool   fleet_mac_excluded(const uint8_t mac[6], uint32_t now_ms);
size_t fleet_peer_count(uint32_t now_ms);        // non-expired entries (tests/diag)

// Note a live peer NODE (its real ESP-NOW hardware sender MAC -- info->src_addr on receipt of a
// FLEET_MACS broadcast), refreshing if already known. Separate table from the synthetic-MAC
// exclusion table above (different purpose: node IDENTITY, not MAC content).
void   fleet_note_peer_node(const uint8_t mac[6], uint32_t now_ms);
// Distinct non-expired peer NODES heard from recently (does not count this node -- ESP-NOW never
// delivers a station's own transmission to its own receive callback).
size_t fleet_node_count(uint32_t now_ms);

// Wire framing: [uint8 count][count * 6 MAC bytes]. Pure, unit-testable.
size_t fleet_macs_pack(uint8_t *out, size_t out_max, const uint8_t (*macs)[6], size_t n);
size_t fleet_macs_unpack(const uint8_t *in, size_t len, uint8_t (*macs)[6], size_t max);
