#pragma once
#include "learn_record.h"
#include "law3.h"

// Idempotent merge of one record into a (store,count,cap) library, keyed by shape_hash.
// Reinforce if present (saturating count, refresh last_seen, widen interval band); else
// insert, evicting the weakest when full. Returns false iff full and rec is the weakest.
bool learn_merge(learned_template_t *store, size_t *count, size_t cap,
                 const learned_template_t *rec, uint16_t sweep_no);

// Like learn_merge, but a duplicate takes max(reinforce_count) instead of incrementing —
// used on the wire-receive path so re-broadcasts don't inflate a shape's weight.
bool learn_merge_wire(learned_template_t *store, size_t *count, size_t cap,
                      const learned_template_t *rec, uint16_t sweep_no);

#ifndef LEARN_SYNC_TOP_N
#define LEARN_SYNC_TOP_N 64          // smallest decoy store (Shade/C6) — a full-down fits every decoy
#endif

// Copy the n strongest records (by reinforce_count, ties: newer last_seen_sweep) into out.
// Does not mutate store. Returns min(count, n).
size_t learn_top_n(const learned_template_t *store, size_t count, learned_template_t *out, size_t n);

// --- wire chunk framing for the ESP-NOW fleet sync (Phase 2) ---
#define RADAR_TYPE_LEARN_OFFER 3     // decoy -> Vigil: newly learned/reinforced records
#define RADAR_TYPE_LEARN_SYNC  4     // Vigil -> all: merged library chunk
#define LEARN_WIRE_RECS_PER_CHUNK 3  // (218 - hdr) / sizeof(record), with margin

typedef struct __attribute__((packed)) {
    uint16_t lib_version;
    uint8_t  chunk_index;
    uint8_t  chunk_count;
    uint8_t  rec_count;
} learn_chunk_hdr_t;

// Pack up to LEARN_WIRE_RECS_PER_CHUNK records into a radar_wire payload chunk.
// Returns 0 (and *plen <= 218) on success, <0 if nrecs exceeds the chunk capacity.
int learn_wire_pack(uint8_t *payload, size_t *plen, const learned_template_t *recs, uint8_t nrecs,
                    uint16_t lib_version, uint8_t chunk_index, uint8_t chunk_count);
// Unpack a chunk. Returns 0 on a well-formed chunk (recs/nrecs/hdr filled), <0 otherwise.
int learn_wire_unpack(const uint8_t *payload, size_t plen, learned_template_t *recs,
                      uint8_t *nrecs, learn_chunk_hdr_t *hdr);

// Re-validate a wire-received record before merging (never trust the wire):
// budget + Law-3 + shape_hash recompute must all hold.
bool learn_regate(const learned_template_t *rec);
