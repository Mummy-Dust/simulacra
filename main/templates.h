#pragma once
#include <stddef.h>
#include <stdint.h>

// A device "archetype" — a self-contained bundle binding vendor/format, interval band, and
// (optional) name together, so generation can never produce an impossible combination.
typedef enum {
    FMT_VENDOR_MFG,     // company_id + structured blob (earbuds / fitness / sensor)
    FMT_IBEACON,        // 4C 00 02 15 + UUID + major + minor + tx
    FMT_EDDYSTONE_UID,  // svc-data 0xFEAA frame 0x00
    FMT_EDDYSTONE_URL,  // svc-data 0xFEAA frame 0x10
    FMT_SVC_TRACKER,    // service-data tracker (Tile 0xFEED)
} fmt_family_t;

typedef struct {
    const char  *archetype;    // debug/inspection label
    fmt_family_t family;
    uint16_t     company_id;   // vendor-mfg family (0 otherwise)
    uint16_t     svc_uuid;     // service-data families (0xFEAA / 0xFEED)
    const char  *name;         // friendly name (NULL = nameless)
    uint8_t      name_prob;    // % chance to attach the name (0 = never)
    uint16_t     itvl_min_ms;  // joint interval band
    uint16_t     itvl_max_ms;
    uint8_t      weight;       // mix proportion (relative)
} device_template_t;

size_t                   templates_count(void);
const device_template_t *template_at(size_t i);
const device_template_t *templates_pick(void);   // weighted by .weight

// Render template `t` into a frozen advertisement: serialized AD bytes (<=31), on-air interval,
// and the company id (0 for service-data families). Returns 0 on success, nonzero if the fields
// failed to serialize (e.g. over the 31-byte budget).
int template_build(const device_template_t *t, uint8_t out_payload[31], uint8_t *out_len,
                   uint16_t *out_itvl_ms, uint16_t *out_company_id);

// Build a generic-but-valid vendor manufacturer-data advertisement for an arbitrary company id
// (for model-driven generation of vendors with no specific template). Returns 0 on success.
int template_build_vendor_mfg(uint16_t company_id, uint8_t out_payload[31], uint8_t *out_len);
