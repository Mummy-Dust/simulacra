#pragma once
#include <stddef.h>
#include <stdint.h>
#include "threat_sig.h"
uint16_t sig_seed_version(void);
size_t   sig_seed_count(void);
size_t   sig_seed_copy(threat_sig_t *out, size_t max);   // returns min(count, max)
