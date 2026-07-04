#pragma once
#include <stdint.h>
#include <stddef.h>
#include "threat_sig.h"

#define RADAR_TYPE_SIG_SYNC 5          // Vigil -> all: signature DB chunk
#define SIG_WIRE_RECS_PER_CHUNK 4      // (218 - hdr) / sizeof(threat_sig_t)

typedef struct __attribute__((packed)) {
    uint16_t content_version;
    uint8_t  chunk_index;
    uint8_t  chunk_count;
    uint8_t  rec_count;
} sig_chunk_hdr_t;

// Pack up to SIG_WIRE_RECS_PER_CHUNK records into a radar_wire payload chunk.
// Returns 0 (and *plen <= 218) on success, <0 if nrecs exceeds the chunk capacity.
int sig_wire_pack(uint8_t *payload, size_t *plen, const threat_sig_t *recs, uint8_t nrecs,
                  uint16_t content_version, uint8_t chunk_index, uint8_t chunk_count);
// Unpack a chunk. Returns 0 on a well-formed chunk (recs/nrecs/hdr filled), <0 otherwise.
int sig_wire_unpack(const uint8_t *payload, size_t plen, threat_sig_t *recs,
                    uint8_t *nrecs, sig_chunk_hdr_t *hdr);
