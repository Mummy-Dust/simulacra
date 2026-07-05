#include <string.h>
#include "sdkconfig.h"
#include "learn.h"
#include "law3.h"
#include "learn_wire.h"     // learn_merge (shared)
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "learn";

// AD type constants
#define AD_FLAGS        0x01
#define AD_UUID16_INC   0x02
#define AD_UUID16_CMP   0x03
#define AD_NAME_SHORT   0x08
#define AD_NAME_CMP     0x09
#define AD_TXPOWER      0x0A
#define AD_APPEARANCE   0x19
#define AD_SVCDATA16    0x16
#define AD_MFG          0xFF

// --- state ---
static learned_template_t s_store[LEARN_CAP];
static size_t             s_count;

typedef struct {
    uint32_t          mac_hash;
    uint32_t          last_ms;
    uint16_t          sightings;
    bool              used;
    bool              promoted;
    uint16_t          itvl_min_ms, itvl_max_ms;
    learned_template_t skel;      // stripped once on first sighting
    bool              valid;      // strip succeeded
} learn_cand_t;

static learn_cand_t s_cand[LEARN_CAND_SLOTS];
static uint16_t     s_sweep;

// ============================ identity-strip =============================

static void mask_range(uint32_t *m, uint8_t from, uint8_t to)  // [from,to)
{ for (uint8_t i = from; i < to && i < 31; i++) *m |= (1u << i); }

bool learn_strip(const uint8_t *ad, uint8_t len, uint16_t company,
                 learned_template_t *out)
{
    if (!ad || len == 0 || len > 31) return false;
    if (law3_forbidden(ad, len)) return false;

    memset(out, 0, sizeof(*out));
    memcpy(out->ad, ad, len);
    out->ad_len = len;
    out->company_id = company;

    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t l = ad[i];
        if (l == 0) break;                               // trailing zero padding: end of AD
        if (i + 1 + l > len) return false;               // overrun => genuinely malformed, reject
        uint8_t type = ad[i + 1];
        uint8_t vfrom = i + 2;                            // first value byte
        uint8_t vto   = i + 1 + l;                        // one past last
        switch (type) {
            case AD_FLAGS: case AD_UUID16_INC: case AD_UUID16_CMP:
            case AD_TXPOWER: case AD_APPEARANCE:
                break;                                    // keep verbatim
            case AD_NAME_SHORT: case AD_NAME_CMP:
                out->name_off = vfrom;                    // replaced at render, not masked
                out->name_len = (uint8_t)(vto - vfrom);
                break;
            case AD_MFG:
                if (vto - vfrom >= 2) {                   // keep company id, mask blob
                    // iBeacon: keep 4C 00 02 15 prefix, mask the rest
                    if (ad[vfrom] == 0x4C && ad[vfrom+1] == 0x00 &&
                        (vto - vfrom) >= 4 && ad[vfrom+2] == 0x02 && ad[vfrom+3] == 0x15)
                        mask_range(&out->rand_mask, vfrom + 4, vto);
                    else
                        mask_range(&out->rand_mask, vfrom + 2, vto);
                } else return false;
                break;
            case AD_SVCDATA16:
                if (vto - vfrom >= 2) {
                    out->svc_uuid = (uint16_t)(ad[vfrom] | (ad[vfrom+1] << 8));
                    mask_range(&out->rand_mask, vfrom + 2, vto);
                } else return false;
                break;
            default:                                      // unknown: keep shape, mask value
                mask_range(&out->rand_mask, vfrom, vto);
                break;
        }
        i += 1 + l;
    }
    // best-effort family (metadata for generation matching)
    if (company == 0x004C) out->family = FMT_IBEACON;
    else if (out->svc_uuid == 0xFEAA) out->family = FMT_EDDYSTONE_UID;
    else if (out->svc_uuid == 0xFEED) out->family = FMT_SVC_TRACKER;
    else out->family = FMT_VENDOR_MFG;
    return true;
}

// ============================ hash + render =============================

// Realistic synthetic device names (never the old generic-word + random-digit pad, which read as
// "Beat1701876" -- an adversary-visible decoy tell). Brand-NEUTRAL pool (plausible on any maker),
// plus small brand-keyed pools matched to the captured company_id so a Samsung device gets a
// Samsung-style name (no Apple-name-on-Samsung mismatch). Names are timeless (no version churn) and
// avoid long digit runs.
static const char *NAMES_GENERIC[] = {
    "Buds","Watch","Band","Earbuds","Ear Buds","Fit Band","Sport Buds","Neckband",
    "Smart Band","BT Speaker","Heart Rate","Sound Core","Smart Ring","Power Buds",
    "Run Watch","Sport Watch","Bass Buds","Sound Bar","Mini Speaker","Sleep Sensor",
    "Active Watch","Fitness Band","Wireless Buds","Sport Earbuds","Portable Speaker",
    "Fitness Tracker","Wireless Earbuds","Wireless Sport Buds"
};
static const char *NAMES_SAMSUNG[] = { "Galaxy Buds","Galaxy Watch","Galaxy Fit","Galaxy Buds Pro","Galaxy Buds2" };
static const char *NAMES_APPLE[]   = { "AirPods","AirPods Pro","Apple Watch","AirPods Max" };
static const char *NAMES_BOSE[]    = { "Bose QC","Bose Sport","QuietComfort" };
static const char *NAMES_GARMIN[]  = { "vivosmart","Forerunner","Venu","Instinct" };
static const char *NAMES_SONY[]    = { "LinkBuds","WF Series","WH Series" };
static const struct { uint16_t company; const char *const *names; uint8_t count; } NAME_BRANDS[] = {
    { 0x0075, NAMES_SAMSUNG, 5 }, { 0x004C, NAMES_APPLE, 4 }, { 0x009E, NAMES_BOSE, 3 },
    { 0x0087, NAMES_GARMIN, 4 },  { 0x012D, NAMES_SONY, 3 },
};

// Pick a realistic name for `company`, preferring one at least `min_len` chars (else the longest so
// a non-last name field can always be filled with real characters instead of digit padding).
static const char *pick_name(uint16_t company, uint8_t min_len)
{
    const char *const *pool = NAMES_GENERIC; size_t n = sizeof(NAMES_GENERIC)/sizeof(NAMES_GENERIC[0]);
    for (size_t i = 0; i < sizeof(NAME_BRANDS)/sizeof(NAME_BRANDS[0]); i++)
        if (NAME_BRANDS[i].company == company) { pool = NAME_BRANDS[i].names; n = NAME_BRANDS[i].count; break; }
    uint8_t fit[32]; size_t k = 0; const char *longest = pool[0];
    for (size_t i = 0; i < n && i < 32; i++) {
        if (strlen(pool[i]) >= min_len) fit[k++] = (uint8_t)i;
        if (strlen(pool[i]) > strlen(longest)) longest = pool[i];
    }
    return k ? pool[fit[esp_random() % k]] : longest;
}

// learn_shape_hash now lives in the shared component (learn_wire.c).

int learn_render(const learned_template_t *t, uint8_t out[31],
                 uint8_t *out_len, uint16_t *out_itvl_ms)
{
    memcpy(out, t->ad, t->ad_len);
    for (uint8_t i = 0; i < t->ad_len && i < 31; i++)
        if (t->rand_mask & (1u << i)) out[i] = (uint8_t)(esp_random() & 0xff);
    uint8_t emit_len = t->ad_len;
    if (t->name_len && t->name_off >= 2) {               // realistic synthetic name (never digit-pad)
        bool last = (t->name_off + t->name_len == t->ad_len);   // name is the final AD element?
        uint8_t avail = last ? (uint8_t)(31 - t->name_off) : t->name_len;
        const char *nm = pick_name(t->company_id, last ? 1 : t->name_len);
        uint8_t nl = (uint8_t)strlen(nm);
        uint8_t m = nl < avail ? nl : avail;
        if (last) {                                      // grow/shrink the AD to the name's natural length
            memcpy(out + t->name_off, nm, m);
            out[t->name_off - 2] = (uint8_t)(1 + m);      // element length byte = type(1) + m value bytes
            emit_len = (uint8_t)(t->name_off + m);
        } else {                                         // keep the captured length; pad tail with spaces
            for (uint8_t i = 0; i < t->name_len; i++)
                out[t->name_off + i] = (i < m) ? (uint8_t)nm[i] : ' ';
        }
    }
    *out_len = emit_len;
    uint16_t lo = t->itvl_min_ms, hi = t->itvl_max_ms;
    if (hi < lo) hi = lo;
    *out_itvl_ms = lo + (hi > lo ? (uint16_t)(esp_random() % (hi - lo + 1)) : 0);
    return 0;
}

// ============================== store ==================================

void   learn_reset(void)
{ memset(s_store, 0, sizeof(s_store)); s_count = 0; memset(s_cand, 0, sizeof(s_cand)); s_sweep = 0; }
size_t learn_count(void) { return s_count; }
const learned_template_t *learn_at(size_t i) { return (i < s_count) ? &s_store[i] : NULL; }

bool learn_store_add(const learned_template_t *t, uint16_t sweep_no)
{
    size_t before = s_count;
    bool ok = learn_merge(s_store, &s_count, LEARN_CAP, t, sweep_no);   // shared idempotent merge
    if (ok && s_count > before)
        ESP_LOGW(TAG, "+shape company=0x%04X svc=0x%04X count=%u",
                 t->company_id, t->svc_uuid, (unsigned)s_count);
    return ok;
}

size_t learn_snapshot(learned_template_t *out, size_t max)
{
    size_t n = (s_count < max) ? s_count : max;
    memcpy(out, s_store, n * sizeof(learned_template_t));
    return n;
}

bool learn_ingest_wire(const learned_template_t *rec)
{
    if (!learn_regate(rec)) return false;
    return learn_merge_wire(s_store, &s_count, LEARN_CAP, rec, s_sweep);
}

void learn_age_out(uint16_t sweep_no)
{
    for (size_t i = 0; i < s_count; ) {
        if ((uint16_t)(sweep_no - s_store[i].last_seen_sweep) > LEARN_AGEOUT_SWEEPS) {
            s_store[i] = s_store[--s_count];            // swap-remove
        } else i++;
    }
}

// ========================= candidate pipeline ==========================

void learn_offer(uint32_t mac_hash, const uint8_t *ad, uint8_t len,
                 uint16_t company, uint32_t now_ms)
{
    learn_cand_t *c = NULL, *freep = NULL;
    for (size_t i = 0; i < LEARN_CAND_SLOTS; i++) {
        if (s_cand[i].used && s_cand[i].mac_hash == mac_hash) { c = &s_cand[i]; break; }
        if (!s_cand[i].used && !freep) freep = &s_cand[i];
    }
    if (!c) {                                        // new candidate
        if (!freep) return;                          // table full: drop this sweep
        c = freep;
        memset(c, 0, sizeof(*c));
        c->used = true; c->mac_hash = mac_hash; c->last_ms = now_ms; c->sightings = 1;
        c->valid = learn_strip(ad, len, company, &c->skel);   // reject => valid=false
        return;
    }
    // repeat sighting: interval sample + count
    int32_t itvl = (int32_t)(now_ms - c->last_ms);
    if (itvl > 0 && itvl < 60000) {
        uint16_t v = (uint16_t)itvl;
        if (c->itvl_min_ms == 0 || v < c->itvl_min_ms) c->itvl_min_ms = v;
        if (v > c->itvl_max_ms) c->itvl_max_ms = v;
    }
    c->last_ms = now_ms;
    if (c->sightings < 0xFFFF) c->sightings++;
    if (c->valid && !c->promoted && c->sightings >= LEARN_MIN_SIGHTINGS) {
        c->skel.itvl_min_ms = c->itvl_min_ms ? c->itvl_min_ms : 100;
        c->skel.itvl_max_ms = c->itvl_max_ms ? c->itvl_max_ms : 200;
        c->skel.shape_hash  = learn_shape_hash(&c->skel);
        learn_store_add(&c->skel, s_sweep);
        c->promoted = true;
    }
}

void learn_end_sweep(uint16_t sweep_no)
{
    s_sweep = sweep_no;
    learn_age_out(sweep_no);
    memset(s_cand, 0, sizeof(s_cand));               // wipe transient candidates
}

// ============================== NVS ====================================
// On-NVS layout: [uint32 magic][uint16 version][uint16 count][count * learned_template_t]

int learn_save_nvs(void)
{
    size_t bytes = 8 + s_count * sizeof(learned_template_t);
    static uint8_t buf[8 + LEARN_CAP * sizeof(learned_template_t)];
    uint32_t magic = LEARN_DB_MAGIC; uint16_t ver = LEARN_DB_VERSION, cnt = (uint16_t)s_count;
    memcpy(buf, &magic, 4); memcpy(buf + 4, &ver, 2); memcpy(buf + 6, &cnt, 2);
    memcpy(buf + 8, s_store, s_count * sizeof(learned_template_t));

    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READWRITE, &h);
    if (e != ESP_OK) return (int)e;
    e = nvs_set_blob(h, "learn_db", buf, bytes);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (int)e;
}

int learn_load_nvs(void)
{
    static uint8_t buf[8 + LEARN_CAP * sizeof(learned_template_t)];
    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READONLY, &h);
    if (e != ESP_OK) return (int)e;
    size_t len = sizeof(buf);
    e = nvs_get_blob(h, "learn_db", buf, &len);
    nvs_close(h);
    if (e != ESP_OK || len < 8) return -1;
    uint32_t magic; uint16_t ver, cnt;
    memcpy(&magic, buf, 4); memcpy(&ver, buf + 4, 2); memcpy(&cnt, buf + 6, 2);
    if (magic != LEARN_DB_MAGIC || ver != LEARN_DB_VERSION) return -1;
    if (cnt > LEARN_CAP || len != 8u + (size_t)cnt * sizeof(learned_template_t)) return -1;
    memcpy(s_store, buf + 8, (size_t)cnt * sizeof(learned_template_t));
    s_count = cnt;
    return 0;
}
