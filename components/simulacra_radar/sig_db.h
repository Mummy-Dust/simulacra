#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "threat_sig.h"

#define SIG_DB_LABEL "simulacra-sigdb-v1"
void sig_db_derive_key(const uint8_t psk[32], uint8_t out_key[32]);

#define SIG_DB_MAGIC   0x53494731u   // "SIG1"
#define SIG_DB_FMT_VER 1

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format_version;
    uint16_t content_version;
    uint16_t count;
    uint8_t  nonce[12];
    uint8_t  tag[16];
} sig_db_hdr_t;

// Seal records -> [hdr | AES-256-GCM ct]. Random nonce; returns 0, <0 on error/over-cap.
int sig_db_seal(uint8_t *out, size_t *out_len, const threat_sig_t *recs,
                uint16_t count, uint16_t content_version, const uint8_t key[32]);
// Open + authenticate. Returns 0 (recs/count/content_version filled), <0 on bad hdr / tag failure.
int sig_db_open(const uint8_t *buf, size_t len, threat_sig_t *recs,
                uint16_t *count, uint16_t *content_version, const uint8_t key[32]);
