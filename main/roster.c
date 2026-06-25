#include "roster.h"
#include "templates.h"
#include "esp_random.h"

static identity_t s_roster[CHURN_ROSTER_SIZE];
static size_t     s_cursor;

// Build a valid random-static address: 6 random bytes with the two most
// significant bits set. Regenerates the astronomically rare all-zero / all-ones
// random part that NimBLE would reject.
static void make_random_static_addr(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[5] |= 0xc0;
        int ones = __builtin_popcount(out[5] & 0x3f);
        for (int i = 0; i < 5; i++) ones += __builtin_popcount(out[i]);
        if (ones != 0 && ones != 46) return;
    }
}

// Build every identity from a randomly-picked archetype bundle (templates.c).
// Each identity freezes a coherent vendor+interval+payload triple; the template
// encoders are the single place format correctness and Law 3 are enforced.
void roster_init(void)
{
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = &s_roster[i];
        make_random_static_addr(id->addr);
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
        id->state = ID_IDLE; id->active_until_ms = 0; id->eligible_at_ms = 0;
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
