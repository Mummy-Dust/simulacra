#pragma once
#include <stdint.h>
#include <stdbool.h>

// Guaranteed-unique identity allocator. A ring of the last UNIQ_HISTORY issued 6-byte
// addresses, shared across both radios (BLE + Wi-Fi). Every fresh address is drawn through
// uniq_try so no freshly-emitted identity collides with any live-or-recent one. UNIQ_HISTORY
// is sized well above the max concurrent live population, so every live address is always
// still in the ring (one structure covers "not live" and "not recently retired").
void uniq_reset(void);                       // clear history (host tests / cold boot)
bool uniq_try(const uint8_t addr[6]);        // unseen -> record + true; duplicate -> false
