#include "roster.h"
#include "templates.h"
#include "rf_model.h"
#include "generate.h"
#include "trace.h"
#include "esp_random.h"
#include "uniq_id.h"

static identity_t s_roster[CHURN_ROSTER_SIZE];
static size_t     s_cursor;

// Build a valid random address whose two most-significant bits are `top2`
// (0xC0 = static-random, 0x40 = resolvable-private/RPA-looking, 0x00 = non-resolvable-private).
// 6 random bytes; regenerates the astronomically rare all-zero / all-ones random part that
// NimBLE would reject. All subtypes are RANDOM addresses, so no real MAC is ever exposed.
void make_random_addr(uint8_t out[6], uint8_t top2)
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[5] = (uint8_t)((out[5] & 0x3f) | (top2 & 0xc0));
        int ones = __builtin_popcount(out[5] & 0x3f);
        for (int i = 0; i < 5; i++) ones += __builtin_popcount(out[i]);
        if (ones == 0 || ones == 46) continue;   // NimBLE rejects all-zero/all-ones
        if (uniq_try(out)) return;                // guaranteed-unique across live + recent history
    }
}

void make_random_static_addr_pub(uint8_t out[6]) { make_random_addr(out, 0xc0); }

// Address-type mix the decoy fleet presents. A real BLE crowd blends random-static peripherals,
// RPA-rotating phones, and NRPA/other; presenting 100% static-random was the leading tell
// (see tools/decoy_audit -> address_type_mix). Weights are a representative default calibrated
// against the bench reference (~52% static / 36% RPA / 12% NRPA).
static const uint8_t  ATYPE_TOP2[3] = { 0xc0, 0x40, 0x00 };   // static, RPA, NRPA (bits 6-7)
static const uint16_t ATYPE_W[3]    = {   52,   36,   12 };

// Fill a random address, choosing its subtype by the fleet mix above.
void make_random_addr_mixed(uint8_t out[6])
{
    uint16_t r = (uint16_t)(esp_random() % (ATYPE_W[0] + ATYPE_W[1] + ATYPE_W[2]));
    uint8_t top2 = 0xc0;
    for (int i = 0; i < 3; i++) { if (r < ATYPE_W[i]) { top2 = ATYPE_TOP2[i]; break; } r -= ATYPE_W[i]; }
    make_random_addr(out, top2);
}

// M4 fallback: build every identity from a randomly-picked archetype bundle (templates.c).
// Used when no usable observed model is present (fresh / never-observed device).
static void roster_fill_from_templates(void)
{
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = &s_roster[i];
        make_random_addr_mixed(id->addr);
        const device_template_t *t = templates_pick();
        uint16_t itvl = 0, cid = 0;
        if (template_build(t, id->payload, &id->payload_len, &itvl, &cid) != 0) {
            id->payload_len = 0;            // serialization guard; self-test catches this
        }
        id->company_id = cid;
        id->adv_itvl_ms = itvl;
        // record which TEMPLATES[] row this is (for inspection/test)
        id->archetype_idx = 0;
        for (size_t k = 0; k < templates_count(); k++)
            if (template_at(k) == t) { id->archetype_idx = (uint8_t)k; break; }
        id->tx_power = 0;
        id->state = ID_IDLE; id->active_until_ms = 0; id->eligible_at_ms = 0;
    }
}

// M6: build the roster by sampling the observed model when one is present and dense enough;
// otherwise fall back to the static template population.
void roster_init(void)
{
    rf_model_t m;
    if (rf_model_load_nvs(&m) == 0 && m.total_obs >= GEN_MIN_OBS) {
#if SIMULACRA_TRACE
        rf_model_dump(&m);                                   // trace: the loaded vendor histogram
#endif
        generate_roster(&m, s_roster, CHURN_ROSTER_SIZE);
        generate_dump_roster(s_roster, CHURN_ROSTER_SIZE);   // acceptance/inspection
    } else {
        roster_fill_from_templates();
    }
    s_cursor = 0;
}

identity_t *roster_promote_candidate(uint32_t now_ms)
{
    for (size_t k = 0; k < CHURN_ROSTER_SIZE; k++) {
        size_t i = (s_cursor + k) % CHURN_ROSTER_SIZE;
        identity_t *id = &s_roster[i];
        if (id->state == ID_IDLE ||
            (id->state == ID_COOLDOWN && now_ms >= id->eligible_at_ms)) {
            id->state = ID_IDLE;
            s_cursor = (i + 1) % CHURN_ROSTER_SIZE;
            return id;
        }
    }
    return NULL;
}

size_t roster_count_in_state(id_state_t s)
{
    size_t n = 0;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) if (s_roster[i].state == s) n++;
    return n;
}

identity_t *roster_at(size_t i) { return &s_roster[i]; }

identity_t *roster_pick_company(uint16_t company_id)
{
    // Uniform-random match (reservoir sampling, one pass) -- NOT always-first -- so multiple
    // same-vendor personas get diverse randomized payloads/intervals, never byte-identical clones.
    identity_t *pick = NULL; size_t seen = 0;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++)
        if (s_roster[i].company_id == company_id && (esp_random() % (++seen)) == 0)
            pick = &s_roster[i];
    return pick;   // NULL if no match
}

void roster_reseed_idle(const rf_model_t *m)
{
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        if (s_roster[i].state != ID_IDLE) continue;
        generate_roster(m, &s_roster[i], 1);   // fills MAC/payload/itvl and sets state = ID_IDLE
    }
}
