#include "drift.h"
#include <math.h>

// Fraction of all observations attributed to company_id in model m (0 if absent).
static float vendor_frac(const rf_model_t *m, uint16_t company_id)
{
    uint64_t total = 0;
    for (int i = 0; i < RF_VENDOR_SLOTS; i++) total += m->vendors[i].count;
    total += m->other_count;
    if (total == 0) return 0.0f;
    if (company_id == RF_VENDOR_UNKNOWN) return (float)m->other_count / (float)total;
    int idx = rf_vendor_index(m, company_id);
    if (idx < 0) return 0.0f;
    return (float)m->vendors[idx].count / (float)total;
}

float drift_score(const rf_model_t *prev, const rf_model_t *cur)
{
    // Vendor-mix L1 distance over the union of company ids (+ the no-mfg "other" bucket).
    float l1 = 0.0f;
    for (int i = 0; i < RF_VENDOR_SLOTS; i++)
        if (prev->vendors[i].count) {
            uint16_t c = prev->vendors[i].company_id;
            l1 += fabsf(vendor_frac(prev, c) - vendor_frac(cur, c));
        }
    for (int i = 0; i < RF_VENDOR_SLOTS; i++)
        if (cur->vendors[i].count) {
            uint16_t c = cur->vendors[i].company_id;
            if (rf_vendor_index(prev, c) < 0)          // not already counted from prev
                l1 += fabsf(vendor_frac(prev, c) - vendor_frac(cur, c));
        }
    l1 += fabsf(vendor_frac(prev, RF_VENDOR_UNKNOWN) - vendor_frac(cur, RF_VENDOR_UNKNOWN));
    float mix = l1 * 0.5f;                              // L1 of two distributions is 0..2 -> 0..1

    // Normalized distinct-device (population) delta, 0..1.
    float a = prev->pop_ewma, b = cur->pop_ewma;
    float denom = (a > b) ? a : b;
    float pop = (denom > 0.0f) ? fabsf(a - b) / denom : 0.0f;

    return 0.7f * mix + 0.3f * pop;                     // mix dominant, population secondary
}

bool drift_exceeds(float score, float threshold) { return score > threshold; }
