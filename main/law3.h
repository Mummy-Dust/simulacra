#pragma once
#include <stdint.h>
#include <stdbool.h>

// True if the payload carries an Apple mfg-data pop-up subtype: 0x07 proximity
// pairing, 0x0F nearby action, or 0x12 Find My. iBeacon (0x02) is NOT flagged.
bool has_apple_popup_subtype(const uint8_t *p, uint8_t len);

// True if the serialized AD (TLV) carries any Law-3 forbidden subtype:
// Apple pop-up (above), Microsoft Swift Pair (mfg company 0x0006), or Google
// Fast Pair (16-bit service data for UUID 0xFE2C).
bool law3_forbidden(const uint8_t *ad, uint8_t len);
