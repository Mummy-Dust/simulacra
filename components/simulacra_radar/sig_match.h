#pragma once
#include "threat_sig.h"

// Returns true iff any signature matches; fills *out with the first match.
bool sig_match(const sig_adv_fields_t *adv, const threat_sig_t *db, size_t count, sig_hit_t *out);

// Map a NimBLE addr type + raw MAC to one SIG_ADDR_* bit. Random addresses are
// classified by the top two bits of the MSB (val[5]): 0b11 static, 0b01 RPA, 0b00 NRPA.
uint8_t sig_addr_type_from(uint8_t nimble_addr_type, const uint8_t mac[6]);

// Re-validate a signature before trusting it (bounds + range). true = safe to use.
bool sig_regate(const threat_sig_t *s);
