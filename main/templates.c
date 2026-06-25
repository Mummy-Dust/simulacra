#include <string.h>
#include "templates.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

// --- the bundle library (M4: hand-written; M6 will learn the real mix) ---
static const device_template_t TEMPLATES[] = {
    // archetype      family            company svc    name           np  imin imax  w
    { "earbuds-sams", FMT_VENDOR_MFG,   0x0075, 0,     "Galaxy Buds", 60, 120, 180, 12 },
    { "earbuds-bose", FMT_VENDOR_MFG,   0x009E, 0,     "Bose QC",     60, 120, 180,  6 },
    { "earbuds-sony", FMT_VENDOR_MFG,   0x012D, 0,     NULL,          40, 120, 180,  4 },
    { "fitness-grmn", FMT_VENDOR_MFG,   0x0087, 0,     "vivosmart",   50, 900,1100, 14 },
    { "sensor-nordic",FMT_VENDOR_MFG,   0x0059, 0,     NULL,           0,1800,2200, 18 },
    { "ibeacon",      FMT_IBEACON,      0x004C, 0,     NULL,           0,  90, 110, 16 },
    { "eddy-uid",     FMT_EDDYSTONE_UID,0,      0xFEAA, NULL,          0,  90, 110, 10 },
    { "eddy-url",     FMT_EDDYSTONE_URL,0,      0xFEAA, NULL,          0, 650, 750,  6 },
    // a tracker row is added in Task 4.
};

static uint16_t rnd_range(uint16_t lo, uint16_t hi) { return lo + (esp_random() % (hi - lo + 1)); }
static uint8_t  rnd_byte(void) { return (uint8_t)(esp_random() & 0xff); }

size_t templates_count(void) { return sizeof(TEMPLATES) / sizeof(TEMPLATES[0]); }
const device_template_t *template_at(size_t i) { return &TEMPLATES[i]; }

const device_template_t *templates_pick(void)
{
    uint32_t total = 0;
    for (size_t i = 0; i < templates_count(); i++) total += TEMPLATES[i].weight;
    uint32_t r = esp_random() % total;
    for (size_t i = 0; i < templates_count(); i++) {
        if (r < TEMPLATES[i].weight) return &TEMPLATES[i];
        r -= TEMPLATES[i].weight;
    }
    return &TEMPLATES[0];
}

// mfg buffer: company(2) + model + status + battery + 1-3 plausible bytes
static void enc_vendor_mfg(const device_template_t *t, struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    mfg[0] = (uint8_t)(t->company_id & 0xff);
    mfg[1] = (uint8_t)((t->company_id >> 8) & 0xff);
    mfg[2] = rnd_byte();                       // model/type
    mfg[3] = (uint8_t)(rnd_byte() & 0x0f);     // status flags
    mfg[4] = (uint8_t)(esp_random() % 101);    // battery 0-100
    uint8_t extra = (uint8_t)(1 + (esp_random() % 3));
    for (uint8_t i = 0; i < extra; i++) mfg[5 + i] = rnd_byte();
    f->mfg_data = mfg;
    f->mfg_data_len = (uint8_t)(5 + extra);
}

// iBeacon: Apple company 4C 00, type 0x02, length 0x15, then UUID + major + minor + tx power.
// Hardcoded prefix => can never drift into a Continuity pop-up subtype (refined Law 3).
static void enc_ibeacon(struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    mfg[0] = 0x4C; mfg[1] = 0x00;          // Apple company id
    mfg[2] = 0x02; mfg[3] = 0x15;          // iBeacon type, length 21
    for (int i = 0; i < 16; i++) mfg[4 + i]  = rnd_byte();   // proximity UUID
    for (int i = 0; i < 4;  i++) mfg[20 + i] = rnd_byte();   // major + minor
    mfg[24] = 0xC5;                         // measured power, -59 dBm
    f->mfg_data = mfg; f->mfg_data_len = 25;
}

// Eddystone advertises under the 16-bit service UUID 0xFEAA, with the frame payload
// carried as service data for the same UUID (frame byte selects UID / URL / TLM / ...).
static const ble_uuid16_t EDDY_UUID = BLE_UUID16_INIT(0xFEAA);

static void enc_eddystone_uid(struct ble_hs_adv_fields *f, uint8_t *sd)
{
    sd[0] = 0xAA; sd[1] = 0xFE;             // service UUID (little-endian)
    sd[2] = 0x00;                           // frame type: UID
    sd[3] = 0xC5;                           // ranging tx power
    for (int i = 0; i < 10; i++) sd[4 + i]  = rnd_byte();  // namespace
    for (int i = 0; i < 6;  i++) sd[14 + i] = rnd_byte();  // instance
    sd[20] = 0x00; sd[21] = 0x00;           // reserved
    f->uuids16 = &EDDY_UUID; f->num_uuids16 = 1; f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = sd; f->svc_data_uuid16_len = 22;
}

// Eddystone-URL: scheme 0x03 = "https://", expansion byte 0x07 = ".com/"
static void enc_eddystone_url(struct ble_hs_adv_fields *f, uint8_t *sd)
{
    static const char *hosts[] = { "example", "acme", "store", "venue" };
    const char *h = hosts[esp_random() % 4];
    uint8_t n = 0;
    sd[n++] = 0xAA; sd[n++] = 0xFE;         // service UUID
    sd[n++] = 0x10;                         // frame type: URL
    sd[n++] = 0xC5;                         // tx power
    sd[n++] = 0x03;                         // scheme https://
    for (const char *c = h; *c; c++) sd[n++] = (uint8_t)*c;
    sd[n++] = 0x07;                         // .com/
    f->uuids16 = &EDDY_UUID; f->num_uuids16 = 1; f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = sd; f->svc_data_uuid16_len = n;
}

int template_build(const device_template_t *t, uint8_t out_payload[31], uint8_t *out_len,
                   uint16_t *out_itvl_ms, uint16_t *out_company_id)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    static uint8_t scratch[31];  // mfg/svc-data working buffer (single task, not reentrant)
    switch (t->family) {
        case FMT_VENDOR_MFG:    enc_vendor_mfg(t, &f, scratch); break;
        case FMT_IBEACON:       enc_ibeacon(&f, scratch); break;
        case FMT_EDDYSTONE_UID: enc_eddystone_uid(&f, scratch); break;
        case FMT_EDDYSTONE_URL: enc_eddystone_url(&f, scratch); break;
        default: *out_len = 0; return 1;   // unimplemented families (Task 4) -> RED
    }

    if (t->name && (esp_random() % 100) < t->name_prob) {
        f.name = (uint8_t *)t->name;
        f.name_len = (uint8_t)strlen(t->name);
        f.name_is_complete = 1;
    }
    // No separate TX-power AD: beacons carry measured power inside their own payload,
    // and it would push iBeacon (flags 3 + mfg 27) over the 31-byte budget.

    uint8_t buf[BLE_HS_ADV_MAX_SZ], len = 0;
    if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) { *out_len = 0; return 1; }
    memcpy(out_payload, buf, len);
    *out_len = len;
    *out_itvl_ms = rnd_range(t->itvl_min_ms, t->itvl_max_ms);
    *out_company_id = t->company_id;
    return 0;
}
