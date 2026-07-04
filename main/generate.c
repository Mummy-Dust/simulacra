#include <string.h>
#include "generate.h"
#include "templates.h"
#include "churn.h"          // CHURN_ACTIVE_SET
#include "roster.h"         // make_random_static_addr_pub
#include "learn.h"          // learned templates (self-learning)
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "generate";

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
    // Prefer a learned shape for this company when one exists (adds real-world variety).
    // archetype_idx offset scheme: >= templates_count() means learned[idx - templates_count()].
    if (learn_count() > 0) {
        size_t cand[LEARN_CAP]; size_t k = 0;
        for (size_t i = 0; i < learn_count(); i++) {
            const learned_template_t *lt = learn_at(i);
            bool match = (company == RF_VENDOR_UNKNOWN) ? (lt->company_id == 0)
                                                        : (lt->company_id == company);
            if (match) cand[k++] = i;
        }
        if (k > 0) {
            size_t pick = cand[esp_random() % k];
            uint16_t itvl;
            if (learn_render(learn_at(pick), out, len, &itvl) == 0) {
                *arch_idx = (uint8_t)(templates_count() + pick);
                return 0;
            }
        }
    }
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

// Draw a diverse built-in template into an identity, avoiding `avoid` (the over-represented company
// we're diversifying away from — the built-in earbuds-sams template is itself 0x0075). Sets
// payload/len/itvl/archetype; returns the on-air company (RF_VENDOR_UNKNOWN for service-data).
static uint16_t diversify_fill(identity_t *id, uint16_t avoid)
{
    const device_template_t *t = templates_pick();
    for (int a = 0; a < 8 && t->company_id == avoid; a++) t = templates_pick();
    uint16_t itvl = 0, cid = 0;
    if (template_build(t, id->payload, &id->payload_len, &itvl, &cid) != 0) id->payload_len = 0;
    id->archetype_idx = 0;
    for (size_t i = 0; i < templates_count(); i++) if (template_at(i) == t) { id->archetype_idx = (uint8_t)i; break; }
    id->adv_itvl_ms = itvl ? itvl : (uint16_t)(100 + (esp_random() % 200));
    return cid ? cid : RF_VENDOR_UNKNOWN;
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
    uint64_t total_w = 0; for (size_t i=0;i<k;i++) total_w += counts[i];

    size_t built = 0;
    for (size_t r=0;r<n;r++){
        identity_t *id=&roster[r];
        make_random_static_addr_pub(id->addr);
        int vi = (k>0)? weighted_pick(counts,k) : -1;
        uint16_t company = (vi>=0)? ids[vi] : RF_VENDOR_UNKNOWN;

        // Diversity floor (per-identity, proportional, stateless -> works for bulk build AND
        // single-identity reseed, with no clustering). If the sampled vendor is over-represented in
        // the model (> GEN_MAX_VENDOR_PCT of observations), redirect a proportional fraction of its
        // draws to a varied built-in template so a monoculture model can't yield a monoculture crowd.
        bool redirect = false;
        if (vi >= 0 && total_w > 0) {
            uint64_t num = (uint64_t)counts[vi] * 100;
            uint64_t floor = (uint64_t)GEN_MAX_VENDOR_PCT * total_w;
            if (num > floor && ((uint64_t)esp_random() % num) < (num - floor)) redirect = true;
        }

        if (redirect) {
            company = diversify_fill(id, company);   // sets payload/len/itvl/archetype
        } else {
            uint8_t arch=0;
            if (build_for_vendor(company, id->payload, &id->payload_len, &arch)!=0){ id->payload_len=0; }
            id->archetype_idx = arch;
            uint16_t itvl = 0;
            if (vi>=0 && slot[vi]>=0) itvl = sample_interval(m->vendors[slot[vi]].itvl_bins);
            id->adv_itvl_ms = itvl ? itvl : (uint16_t)(100 + (esp_random()%200));
        }
        id->company_id = company;
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

void generate_dump_roster(const identity_t *roster, size_t n)
{
    uint16_t ids[24]; uint32_t cnt[24]; size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = roster[i].company_id; size_t j;
        for (j = 0; j < k; j++) if (ids[j] == c) { cnt[j]++; break; }
        if (j == k && k < 24) { ids[k] = c; cnt[k] = 1; k++; }
    }
    ESP_LOGW(TAG, "GENERATED roster n=%u distinct_companies=%u", (unsigned)n, (unsigned)k);
    for (size_t j = 0; j < k; j++)
        ESP_LOGW(TAG, "  company 0x%04X x%u", ids[j], (unsigned)cnt[j]);
}
