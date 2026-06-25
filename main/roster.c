#include <stdbool.h>
#include <string.h>
#include "roster.h"
#include "decoy_vendors.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

// Probability (percent) that a generated identity carries a friendly name /
// manufacturer-specific data, mirroring the variety of a real BLE crowd.
#define SIMULACRA_NAME_PROB 60
#define SIMULACRA_MFG_PROB  85

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

// Populate an adv-fields struct for a random vendor from the decoy palette.
// Returns the chosen company id (stored on the identity for inspection). Never
// emits Apple/Microsoft/Google pairing-popup formats — the palette excludes them.
static uint16_t build_fields(struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    const vendor_t *v = &VENDORS[esp_random() % VENDOR_COUNT];
    memset(f, 0, sizeof(*f));
    f->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    bool used_name = false;
    if (v->name && (esp_random() % 100) < SIMULACRA_NAME_PROB) {
        f->name = (uint8_t *)v->name; f->name_len = strlen(v->name);
        f->name_is_complete = 1; used_name = true;
    }
    if ((esp_random() % 100) < SIMULACRA_MFG_PROB) {
        size_t body = used_name ? 3 : (3 + (esp_random() % 5));
        mfg[0] = (uint8_t)(v->company_id & 0xff);
        mfg[1] = (uint8_t)((v->company_id >> 8) & 0xff);
        for (size_t i = 0; i < body; i++) mfg[2 + i] = (uint8_t)(esp_random() & 0xff);
        f->mfg_data = mfg; f->mfg_data_len = 2 + body;
    }
    f->tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO; f->tx_pwr_lvl_is_present = 1;
    return v->company_id;
}

void roster_init(void)
{
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = &s_roster[i];
        make_random_static_addr(id->addr);
        struct ble_hs_adv_fields f; uint8_t mfg[10];
        id->company_id = build_fields(&f, mfg);
        uint8_t buf[BLE_HS_ADV_MAX_SZ], len = 0;
        if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) len = 0;
        memcpy(id->payload, buf, len); id->payload_len = len;
        id->adv_itvl_ms = 100 + (esp_random() % 200);  // 100-300 ms
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
