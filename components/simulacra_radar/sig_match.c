#include "sig_match.h"
#include <string.h>

uint8_t sig_addr_type_from(uint8_t nimble_addr_type, const uint8_t mac[6])
{
    // NimBLE: 0/2 = public(-id), 1/3 = random(-id).
    if (nimble_addr_type == 0 || nimble_addr_type == 2) return SIG_ADDR_PUBLIC;
    switch (mac[5] >> 6) {
        case 0x3: return SIG_ADDR_STATIC;
        case 0x1: return SIG_ADDR_RPA;
        case 0x0: return SIG_ADDR_NRPA;
        default:  return SIG_ADDR_STATIC;   // 0b10 reserved -> treat as static
    }
}

bool sig_regate(const threat_sig_t *s)
{
    if (!s) return false;
    if (s->pat_len > SIG_PAT_MAX) return false;
    if ((int)s->pat_off + (int)s->pat_len > SIG_PAT_MAX) return false;  // pattern[]/mask[] bound
    if (s->category >= SIG_CAT_COUNT) return false;
    if (s->class_id >= SIG_CLASS_COUNT) return false;
    if (s->match_src >= SIG_SRC_COUNT) return false;
    if (s->confidence > 100) return false;
    return true;
}

static bool one_match(const sig_adv_fields_t *adv, const threat_sig_t *s)
{
    if (s->company_id != 0xFFFF && adv->company_id != s->company_id) return false;
    if (s->svc_uuid16 != 0x0000 && adv->svc_uuid16 != s->svc_uuid16) return false;
    if (s->addr_type_mask != 0 && !(s->addr_type_mask & adv->addr_type)) return false;

    const uint8_t *buf; uint8_t len;
    if (s->match_src == SIG_SRC_MFG_DATA) { buf = adv->mfg_data; len = adv->mfg_len; }
    else                                  { buf = adv->svc_data; len = adv->svc_len; }
    if (!buf) return false;
    if ((int)s->pat_off + (int)s->pat_len > (int)len) return false;   // bounds-safe
    for (uint8_t i = 0; i < s->pat_len; i++)
        if ((buf[s->pat_off + i] & s->mask[i]) != (s->pattern[i] & s->mask[i])) return false;
    return true;
}

bool sig_match(const sig_adv_fields_t *adv, const threat_sig_t *db, size_t count, sig_hit_t *out)
{
    if (!adv || !db) return false;
    for (size_t i = 0; i < count; i++) {
        if (one_match(adv, &db[i])) {
            if (out) { out->sig_id = db[i].sig_id; out->category = db[i].category;
                       out->class_id = db[i].class_id; out->confidence = db[i].confidence; }
            return true;
        }
    }
    return false;
}
