#include "sig_seed.h"
#include <string.h>

#define SIG_SEED_VERSION 1

// Starting vectors from public documentation; confirm against a live capture before bench sign-off.
static const threat_sig_t SEED[] = {
    // AirTag / Find My offline finding: Apple 0x004C, mfg type byte 0x12 at off 2 (key bytes masked).
    { .sig_id=1, .category=SIG_CAT_TRACKER, .class_id=SIG_CLASS_AIRTAG,
      .company_id=0x004C, .svc_uuid16=0x0000, .addr_type_mask=SIG_ADDR_RPA|SIG_ADDR_NRPA,
      .match_src=SIG_SRC_MFG_DATA, .pat_off=2, .pat_len=1,
      .pattern={0x12}, .mask={0xFF}, .confidence=80 },
    // Samsung SmartTag: service UUID 0xFD5A (Samsung); may also appear on other Samsung gear -> "possible".
    { .sig_id=2, .category=SIG_CAT_TRACKER, .class_id=SIG_CLASS_SMARTTAG,
      .company_id=0xFFFF, .svc_uuid16=0xFD5A, .addr_type_mask=0,
      .match_src=SIG_SRC_SVC_DATA, .pat_off=0, .pat_len=2,
      .pattern={0x5A,0xFD}, .mask={0xFF,0xFF}, .confidence=70 },
    // Tile: service UUID 0xFEED.
    { .sig_id=3, .category=SIG_CAT_TRACKER, .class_id=SIG_CLASS_TILE,
      .company_id=0xFFFF, .svc_uuid16=0xFEED, .addr_type_mask=0,
      .match_src=SIG_SRC_SVC_DATA, .pat_off=0, .pat_len=2,
      .pattern={0xED,0xFE}, .mask={0xFF,0xFF}, .confidence=75 },
};

uint16_t sig_seed_version(void) { return SIG_SEED_VERSION; }
size_t   sig_seed_count(void)   { return sizeof(SEED)/sizeof(SEED[0]); }
size_t   sig_seed_copy(threat_sig_t *out, size_t max)
{
    size_t n = sig_seed_count(); if (n > max) n = max;
    memcpy(out, SEED, n * sizeof(threat_sig_t));
    return n;
}
