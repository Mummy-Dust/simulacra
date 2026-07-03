#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "learn_record.h"

#define LEARN_DB_LABEL "simulacra-learndb-v1"

// HKDF-SHA256(ikm=psk[32], salt=NULL, info=LEARN_DB_LABEL) -> out_key[32].
void learn_db_derive_key(const uint8_t psk[32], uint8_t out_key[32]);
