#include "law3.h"

// (moved verbatim from churn_selftest.c) Scans for the Apple company prefix
// 4C 00 immediately followed by a forbidden Continuity/Find My subtype byte.
bool has_apple_popup_subtype(const uint8_t *p, uint8_t len)
{
    for (int i = 0; i + 2 < len; i++)
        if (p[i] == 0x4C && p[i+1] == 0x00 &&
            (p[i+2] == 0x07 || p[i+2] == 0x0F || p[i+2] == 0x12)) return true;
    return false;
}

bool law3_forbidden(const uint8_t *ad, uint8_t len)
{
    if (has_apple_popup_subtype(ad, len)) return true;
    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t l = ad[i];
        if (l == 0 || i + 1 + l > len) break;
        const uint8_t *e = &ad[i + 1];        // e[0] = type
        uint8_t t = e[0];
        // Microsoft Swift Pair: mfg-data, company 0x0006
        if (t == 0xFF && l >= 3 && e[1] == 0x06 && e[2] == 0x00) return true;
        // Google Fast Pair: 16-bit service data (0x16) for UUID 0xFE2C
        if (t == 0x16 && l >= 3 && e[1] == 0x2C && e[2] == 0xFE) return true;
        i += 1 + l;
    }
    return false;
}
