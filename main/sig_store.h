#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "threat_sig.h"
#include "sig_seed.h"

void   sig_store_load_seed(void);                 // copy the compile-time seed into the RAM DB
size_t sig_store_count(void);
const threat_sig_t *sig_store_db(void);           // pointer to the internal array (read-only use)
uint16_t sig_store_version(void);
// Re-gate each record; adopt (wholesale swap) iff content_version >= current and >=1 record
// survives re-gate. Returns true if adopted.
bool   sig_store_adopt(const threat_sig_t *recs, size_t n, uint16_t content_version);
