#include <string.h>
#include "generate.h"
#include "templates.h"
#include "churn.h"          // CHURN_ACTIVE_SET
#include "roster.h"         // make_random_static_addr_pub
#include "esp_random.h"

// --- persona profile (factor in tenths, to avoid float in the hot path) ---
#if CONFIG_IDF_TARGET_ESP32C5
#define GEN_FACTOR_X10 15   // Ward: 1.5x
#define GEN_FLOOR      6
#define GEN_CEILING    16
#else
#define GEN_FACTOR_X10 11   // Shade: 1.1x
#define GEN_FLOOR      4
#define GEN_CEILING    8
#endif

// interval bin [lo,hi) edges in ms; the >2000 bin caps at 3000.
static const uint16_t ITVL_LO[RF_ITVL_BINS] = {   0,  50, 100, 200,  500, 1000, 2000 };
static const uint16_t ITVL_HI[RF_ITVL_BINS] = {  50, 100, 200, 500, 1000, 2000, 3000 };

static uint16_t rnd_range16(uint16_t lo, uint16_t hi){ return (hi<=lo)?lo:(lo+(esp_random()%(hi-lo))); }

// Weighted pick over counts[0..n); returns index, or -1 if all zero.
static int weighted_pick(const uint32_t *counts, size_t n)
{
    uint64_t total = 0; for (size_t i=0;i<n;i++) total += counts[i];
    if (total == 0) return -1;
    uint64_t r = (uint64_t)esp_random() % total;
    for (size_t i=0;i<n;i++){ if (r < counts[i]) return (int)i; r -= counts[i]; }
    return (int)n - 1;
}

// Sample an interval (ms) from a vendor slot's histogram; 0 if the slot has no samples.
static uint16_t sample_interval(const uint32_t bins[RF_ITVL_BINS])
{
    int b = weighted_pick(bins, RF_ITVL_BINS);
    if (b < 0) return 0;
    return rnd_range16(ITVL_LO[b], ITVL_HI[b]);
}

// Map a sampled company id -> a built payload + a representative archetype index (always valid).
// 0x004C -> iBeacon; 0xFFFF (no-mfg) -> a beacon/tracker family; a templated company -> its template;
// otherwise a generic vendor-mfg carrying that company id.
static int build_for_vendor(uint16_t company, uint8_t out[31], uint8_t *len, uint8_t *arch_idx)
{
    // observed Apple -> iBeacon (safe subtype; Law 3)
    if (company == 0x004C) {
        for (size_t i = 0; i < templates_count(); i++) {
            const device_template_t *t = template_at(i);
            if (t->family == FMT_IBEACON) {
                uint16_t itvl, cid;
                if (template_build(t, out, len, &itvl, &cid) == 0) { *arch_idx=(uint8_t)i; return 0; }
            }
        }
    }
    // no-mfg observed -> random beacon/tracker template
    if (company == RF_VENDOR_UNKNOWN) {
        for (int tries = 0; tries < 8; tries++) {
            size_t i = esp_random() % templates_count();
            const device_template_t *t = template_at(i);
            if (t->family==FMT_IBEACON || t->family==FMT_EDDYSTONE_UID ||
                t->family==FMT_EDDYSTONE_URL || t->family==FMT_SVC_TRACKER) {
                uint16_t itvl, cid;
                if (template_build(t, out, len, &itvl, &cid)==0){ *arch_idx=(uint8_t)i; return 0; }
            }
        }
    }
    // a templated vendor-mfg company?
    for (size_t i = 0; i < templates_count(); i++) {
        const device_template_t *t = template_at(i);
        if (t->family==FMT_VENDOR_MFG && t->company_id==company) {
            uint16_t itvl, cid;
            if (template_build(t, out, len, &itvl, &cid)==0){ *arch_idx=(uint8_t)i; return 0; }
        }
    }
    // generic vendor-mfg for an arbitrary company; archetype = first vendor-mfg template (valid idx)
    if (template_build_vendor_mfg(company, out, len) == 0) {
        for (size_t i=0;i<templates_count();i++)
            if (template_at(i)->family==FMT_VENDOR_MFG){ *arch_idx=(uint8_t)i; break; }
        return 0;
    }
    return 1;
}

static int8_t dither_tx(void)   // plausible TX spread; not all at max
{
    static const int8_t lv[] = { -12, -9, -6, -3, 0, 3 };   // 0 -> controller default in churn_adv
    return lv[esp_random() % (sizeof(lv)/sizeof(lv[0]))];
}

size_t generate_roster(const rf_model_t *m, identity_t *roster, size_t n)
{
    // build the vendor sampling table: occupied 24 slots + other(no-mfg 0xFFFF)
    uint32_t counts[RF_VENDOR_SLOTS + 1];
    uint16_t ids[RF_VENDOR_SLOTS + 1];
    int      slot[RF_VENDOR_SLOTS + 1];        // back-ref to the vendor slot (-1 = other/no-mfg)
    size_t k = 0;
    for (size_t i=0;i<RF_VENDOR_SLOTS;i++)
        if (m->vendors[i].count){ counts[k]=m->vendors[i].count; ids[k]=m->vendors[i].company_id; slot[k]=(int)i; k++; }
    if (m->other_count){ counts[k]=m->other_count; ids[k]=RF_VENDOR_UNKNOWN; slot[k]=-1; k++; }

    size_t built = 0;
    for (size_t r=0;r<n;r++){
        identity_t *id=&roster[r];
        make_random_static_addr_pub(id->addr);
        int vi = (k>0)? weighted_pick(counts,k) : -1;
        uint16_t company = (vi>=0)? ids[vi] : RF_VENDOR_UNKNOWN;
        uint8_t arch=0;
        if (build_for_vendor(company, id->payload, &id->payload_len, &arch)!=0){ id->payload_len=0; }
        id->company_id = company;
        id->archetype_idx = arch;
        // interval: from the sampled vendor's histogram (else a default 100-300 ms)
        uint16_t itvl = 0;
        if (vi>=0 && slot[vi]>=0) itvl = sample_interval(m->vendors[slot[vi]].itvl_bins);
        id->adv_itvl_ms = itvl ? itvl : (uint16_t)(100 + (esp_random()%200));
        id->tx_power = dither_tx();
        id->state=ID_IDLE; id->active_until_ms=0; id->eligible_at_ms=0;
        if (id->payload_len) built++;
    }
    return built;
}

uint8_t generate_active_target(const rf_model_t *m)
{
    int t = (int)((m->pop_ewma * GEN_FACTOR_X10 + 5) / 10);   // round(pop*factor)
    if (t < GEN_FLOOR) t = GEN_FLOOR;
    if (t > GEN_CEILING) t = GEN_CEILING;
    if (t > CHURN_ACTIVE_SET) t = CHURN_ACTIVE_SET;
    return (uint8_t)t;
}
