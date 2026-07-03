#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// A learned archetype: identity-stripped AD skeleton + metadata. PACKED so the
// byte layout is deterministic across the decoy (RISC-V) and Vigil (Xtensa) for
// memcpy-based wire serialization. Shared by main/learn.c and the Vigil librarian.
typedef struct __attribute__((packed)) {
    uint8_t  ad[31];          // identity-stripped skeleton (serialized AD bytes)
    uint8_t  ad_len;
    uint8_t  name_off;        // offset of local-name VALUE in ad[] (0 = no name)
    uint8_t  name_len;        // length of local-name value (0 = no name)
    uint32_t rand_mask;       // bit i set => rewrite ad[i] with esp_random() per render
    uint16_t company_id;      // 0 if none
    uint16_t svc_uuid;        // 0 if none
    uint8_t  family;          // fmt_family_t best-effort classification
    uint16_t itvl_min_ms;
    uint16_t itvl_max_ms;
    uint32_t shape_hash;
    uint16_t reinforce_count;
    uint16_t last_seen_sweep;
} learned_template_t;

// FNV-1a over family + company_id + svc_uuid + the AD-type/length sequence
// (NOT value bytes) -> dedup / merge key.
uint32_t learn_shape_hash(const learned_template_t *t);
