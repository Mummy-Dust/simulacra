#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "learn_record.h"

#define LEARN_DB_LABEL "simulacra-learndb-v1"

// HKDF-SHA256(ikm=psk[32], salt=NULL, info=LEARN_DB_LABEL) -> out_key[32].
void learn_db_derive_key(const uint8_t psk[32], uint8_t out_key[32]);

#define LEARN_DB_MAGIC   0x4C444231u   // "LDB1"
#define LEARN_DB_FMT_VER 1
#define LEARN_DB_MAX     1024          // Vigil archive ceiling

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint8_t  nonce[12];
    uint8_t  tag[16];
} learn_db_hdr_t;

// Seal records -> [hdr | AES-256-GCM ct]. Random nonce; returns 0, <0 on error/over-cap.
int learn_db_seal(uint8_t *out, size_t *out_len, const learned_template_t *recs,
                  uint16_t count, const uint8_t key[32]);
// Open + authenticate. Returns 0 (recs/count filled), <0 on bad hdr / tag failure.
int learn_db_open(const uint8_t *buf, size_t len, learned_template_t *recs,
                  uint16_t *count, const uint8_t key[32]);
