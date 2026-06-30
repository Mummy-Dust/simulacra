#include <stdbool.h>
#include <string.h>
#include "churn_selftest.h"
#include "roster.h"
#include "churn.h"
#include "templates.h"
#include "rf_model.h"
#include "observe.h"
#include "generate.h"
#include "esp_log.h"

#define GEN_FLOOR_TEST_MIN 4   // lower of the two persona floors (Shade); valid for either build

static const char *TAG = "selftest";
static int s_total, s_fail;
static const char *s_first_fail;

#define ST_CHECK(cond, msg) do { s_total++; if (!(cond)) { \
    s_fail++; if (!s_first_fail) s_first_fail = (msg); \
    ESP_LOGE(TAG, "FAIL: %s", (msg)); } } while (0)

// Recording-apply stubs return int to match churn_apply_fn (= churn_adv_apply).
static int noop_apply(uint8_t instance, const identity_t *id)
{ (void)instance; (void)id; return 0; }

static void test_churn_lifecycle(void)
{
    roster_init();
    churn_set_apply(noop_apply);
    churn_init(0);
    ST_CHECK(churn_active_count() == CHURN_ACTIVE_SET, "init fills the active set");

    // Force slot 0's identity to expire at t=1000, tick, expect replacement.
    const identity_t *before = churn_active_at(0);
    ((identity_t *)before)->active_until_ms = 1000;
    churn_tick(2000);
    const identity_t *after = churn_active_at(0);
    ST_CHECK(after != before, "expired active identity is replaced");
    ST_CHECK(before->state == ID_COOLDOWN, "retired identity enters COOLDOWN");
    ST_CHECK(after && after->state == ID_ACTIVE, "promoted identity is ACTIVE");
}

static void test_cooldown(void)
{
    roster_init();
    churn_set_apply(noop_apply);
    churn_init(0);

    // Retire slot 0 at t=1000; capture its cooldown deadline.
    identity_t *retiring = (identity_t *)churn_active_at(0);
    retiring->active_until_ms = 1000;
    churn_tick(2000);
    ST_CHECK(retiring->state == ID_COOLDOWN, "retired -> COOLDOWN");
    uint32_t elig = retiring->eligible_at_ms;
    ST_CHECK(elig >= 2000 + CHURN_COOLDOWN_MIN_MS, "cooldown >= min");
    ST_CHECK(elig <= 2000 + CHURN_COOLDOWN_MAX_MS, "cooldown <= max");

    // Long simulation: this identity never re-enters ACTIVE before eligible_at.
    bool violated = false;
    for (uint32_t t = 2000; t < 4000000u; t += 1000) {
        churn_tick(t);
        if (t < elig) {
            for (size_t s = 0; s < CHURN_ACTIVE_SET; s++) {
                if (churn_active_at(s) == retiring) violated = true;
            }
        }
    }
    ST_CHECK(!violated, "no MAC reappears within its cooldown window");
}

static const identity_t *s_rec[CHURN_HW_INSTANCES];
static int s_apply_calls;
static int rec_apply(uint8_t instance, const identity_t *id)
{ if (instance < CHURN_HW_INSTANCES) s_rec[instance] = id; s_apply_calls++; return 0; }

static void test_timeslice(void)
{
    roster_init();
    memset(s_rec, 0, sizeof(s_rec));
    s_apply_calls = 0;
    churn_set_apply(rec_apply);
    churn_init(0);
    // Freeze dwell so nobody retires mid-test (deterministic coverage).
    for (int s = 0; s < CHURN_ACTIVE_SET; s++) {
        identity_t *a = (identity_t *)churn_active_at(s);
        if (a) a->active_until_ms = 0xFFFFFFFFu;
    }

    // Collect the identities placed on radios across enough slices to cover the whole active set.
    int slices = (CHURN_ACTIVE_SET + CHURN_HW_INSTANCES - 1) / CHURN_HW_INSTANCES;
    const identity_t *seen[CHURN_ACTIVE_SET * 2]; int n = 0;
    for (int sl = 1; sl <= slices; sl++) {
        churn_tick((uint32_t)sl * CHURN_SLICE_MS);
        for (int i = 0; i < CHURN_HW_INSTANCES; i++) seen[n++] = s_rec[i];
    }

    // Every active identity should appear within ceil(ACTIVE_SET / HW) slices.
    int covered = 0;
    for (int s = 0; s < CHURN_ACTIVE_SET; s++) {
        const identity_t *a = churn_active_at(s);
        for (int j = 0; j < n; j++) if (seen[j] == a) { covered++; break; }
    }
    ST_CHECK(covered == CHURN_ACTIVE_SET,
             "every active identity is on-air within ceil(ACTIVE_SET/HW) slices");
    ST_CHECK(s_apply_calls > 0, "time-slice drives the apply callback");
}

static void test_templates(void)
{
    ST_CHECK(templates_count() >= 5, "template library populated");
    bool all_build = true, all_budget = true, all_itvl = true;
    for (size_t i = 0; i < templates_count(); i++) {
        const device_template_t *t = template_at(i);
        uint8_t pay[31], len = 0; uint16_t itvl = 0, cid = 0;
        int rc = template_build(t, pay, &len, &itvl, &cid);
        if (rc != 0 || len == 0) { all_build = false; continue; }
        if (len > 31) all_budget = false;
        if (itvl < t->itvl_min_ms || itvl > t->itvl_max_ms) all_itvl = false;
    }
    ST_CHECK(all_build, "every template builds a non-empty payload");
    ST_CHECK(all_budget, "every template payload fits 31 bytes");
    ST_CHECK(all_itvl, "every interval is within the template band");
}

static const device_template_t *find_family(fmt_family_t fam)
{
    for (size_t i = 0; i < templates_count(); i++)
        if (template_at(i)->family == fam) return template_at(i);
    return NULL;
}

// Refined Law 3: a payload must never carry an Apple Continuity pop-up subtype
// (0x07 proximity pairing, 0x0F nearby action) or Find My (0x12). iBeacon (0x02) is fine.
static bool has_apple_popup_subtype(const uint8_t *p, uint8_t len)
{
    for (int i = 0; i + 2 < len; i++)
        if (p[i] == 0x4C && p[i+1] == 0x00 &&
            (p[i+2] == 0x07 || p[i+2] == 0x0F || p[i+2] == 0x12)) return true;
    return false;
}

static void test_ibeacon(void)
{
    const device_template_t *t = find_family(FMT_IBEACON);
    ST_CHECK(t != NULL, "iBeacon template present");
    if (!t) return;
    uint8_t pay[31], len = 0; uint16_t itvl = 0, cid = 0;
    ST_CHECK(template_build(t, pay, &len, &itvl, &cid) == 0 && len > 0, "iBeacon builds");
    bool prefix_ok = false;
    for (int i = 0; i + 1 < len; ) {
        uint8_t adlen = pay[i]; uint8_t adtype = pay[i+1];
        if (adtype == 0xFF && adlen >= 5 &&
            pay[i+2] == 0x4C && pay[i+3] == 0x00 && pay[i+4] == 0x02 && pay[i+5] == 0x15)
            prefix_ok = true;
        i += adlen + 1;
    }
    ST_CHECK(prefix_ok, "iBeacon payload carries 4C 00 02 15");
    ST_CHECK(!has_apple_popup_subtype(pay, len), "iBeacon: no forbidden Apple subtype");
}

static bool payload_has_svc_uuid16(const uint8_t *p, uint8_t len, uint16_t uuid)
{
    uint8_t lo = uuid & 0xff, hi = (uuid >> 8) & 0xff;
    for (int i = 0; i + 1 < len; ) {
        uint8_t adlen = p[i], adtype = p[i+1];
        // 0x16 = service data 16-bit; first two data bytes are the UUID (little-endian)
        if (adtype == 0x16 && adlen >= 3 && p[i+2] == lo && p[i+3] == hi) return true;
        i += adlen + 1;
    }
    return false;
}

static void test_eddystone(void)
{
    const device_template_t *u = find_family(FMT_EDDYSTONE_UID);
    const device_template_t *r = find_family(FMT_EDDYSTONE_URL);
    ST_CHECK(u && r, "Eddystone UID+URL templates present");
    uint8_t pay[31], len = 0; uint16_t itvl = 0, cid = 0;
    if (u) {
        ST_CHECK(template_build(u, pay, &len, &itvl, &cid) == 0 && len > 0, "Eddystone-UID builds");
        ST_CHECK(payload_has_svc_uuid16(pay, len, 0xFEAA), "Eddystone-UID svc-data 0xFEAA");
    }
    if (r) {
        ST_CHECK(template_build(r, pay, &len, &itvl, &cid) == 0 && len > 0, "Eddystone-URL builds");
        ST_CHECK(payload_has_svc_uuid16(pay, len, 0xFEAA), "Eddystone-URL svc-data 0xFEAA");
    }
}

static void test_tracker(void)
{
    const device_template_t *t = find_family(FMT_SVC_TRACKER);
    ST_CHECK(t != NULL, "tracker template present");
    if (!t) return;
    uint8_t pay[31], len = 0; uint16_t itvl = 0, cid = 0;
    ST_CHECK(template_build(t, pay, &len, &itvl, &cid) == 0 && len > 0, "tracker builds");
    ST_CHECK(payload_has_svc_uuid16(pay, len, 0xFEED), "tracker svc-data 0xFEED");
}

// Whole-roster structural + Law-3 guard: every identity must be a template-built,
// budget-fitting, non-popup payload on a random-static MAC.
static void test_roster_payloads(void)
{
    roster_init();
    bool macs_ok = true, payload_ok = true, no_popup = true, archetype_ok = true;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = roster_at(i);
        if ((id->addr[5] & 0xc0) != 0xc0) macs_ok = false;
        if (id->payload_len == 0 || id->payload_len > 31) payload_ok = false;
        if (has_apple_popup_subtype(id->payload, id->payload_len)) no_popup = false;
        if (id->archetype_idx >= templates_count()) archetype_ok = false;
    }
    ST_CHECK(macs_ok, "roster: all MACs random-static");
    ST_CHECK(payload_ok, "roster: all payloads non-empty and <=31B");
    ST_CHECK(no_popup, "roster: no forbidden Apple subtype anywhere");
    ST_CHECK(archetype_ok, "roster: every identity carries a valid template index");
}

static void test_rf_model(void)
{
    rf_model_t m;
    rf_model_reset(&m);
    ST_CHECK(m.magic == RF_MODEL_MAGIC && m.version == RF_MODEL_VERSION, "rf_model resets to valid header");
    ST_CHECK(m.total_obs == 0 && m.other_count == 0, "rf_model resets counts to zero");

    // bin edges
    ST_CHECK(rf_itvl_bin(49) == 0 && rf_itvl_bin(50) == 1 && rf_itvl_bin(2001) == 6, "interval bin edges");
    ST_CHECK(rf_rssi_bin(-100) == 0 && rf_rssi_bin(-20) == 7, "rssi bin edges");
    ST_CHECK(rf_pdu_bin(0) == 0 && rf_pdu_bin(4) == 4 && rf_pdu_bin(9) == RF_PDU_BINS - 1, "pdu bin map");

    // first sighting (no interval), then a repeat with a 150 ms interval
    rf_model_observe(&m, 0x004C, -50, 0, -1);
    rf_model_observe(&m, 0x004C, -50, 0, 150);
    int vi = rf_vendor_index(&m, 0x004C);
    ST_CHECK(vi >= 0 && m.vendors[vi].count == 2, "vendor slot counts both observations");
    ST_CHECK(m.vendors[vi].itvl_bins[rf_itvl_bin(150)] == 1, "interval sample lands in bin 2");
    ST_CHECK(m.vendors[vi].itvl_bins[0] == 0, "no spurious interval sample for first sighting");
    ST_CHECK(m.rssi_bins[rf_rssi_bin(-50)] == 2 && m.pdu_bins[0] == 2, "rssi+pdu histograms updated");

    // vendor-table overflow -> other bucket
    rf_model_t o; rf_model_reset(&o);
    for (int i = 0; i < RF_VENDOR_SLOTS; i++) rf_model_observe(&o, (uint16_t)(0x100 + i), -60, 0, -1);
    rf_model_observe(&o, 0x9999, -60, 0, -1);   // 25th distinct vendor
    ST_CHECK(o.other_count == 1, "overflow vendor lands in other bucket");

    // sweep EWMA
    rf_model_t s; rf_model_reset(&s);
    rf_model_end_sweep(&s, 5, 60000, 5);
    ST_CHECK((int)(s.pop_ewma + 0.5f) == 5 && (int)(s.arrival_per_min + 0.5f) == 5, "first sweep seeds EWMA");
}

static void test_observe_dedup(void)
{
    const uint8_t A[6] = {0x01,0x02,0x03,0x04,0x05,0xC0};
    const uint8_t B[6] = {0x11,0x12,0x13,0x14,0x15,0xC1};
    rf_model_t m; rf_model_reset(&m);
    observe_reset_ephemeral(0xDEADBEEF);

    observe_ingest(&m, A, 1000, 0x0075, -40, 0);   // first sighting of A
    observe_ingest(&m, A, 1150, 0x0075, -40, 0);   // repeat -> 150 ms interval
    ST_CHECK(observe_ephemeral_count() == 1, "repeat MAC does not inflate distinct count");
    int vi = rf_vendor_index(&m, 0x0075);
    ST_CHECK(vi >= 0 && m.vendors[vi].itvl_bins[rf_itvl_bin(150)] == 1, "interval estimated from re-sighting");

    observe_ingest(&m, B, 1200, 0x0087, -55, 0);   // a second distinct device
    ST_CHECK(observe_ephemeral_count() == 2, "distinct devices counted");

    observe_end_sweep(&m, 60000);
    ST_CHECK((int)(m.pop_ewma + 0.5f) == 2, "sweep population = distinct devices");
    ST_CHECK((int)(m.arrival_per_min + 0.5f) == 2, "arrival rate folded");
    ST_CHECK(observe_ephemeral_count() == 0, "ephemeral table wiped after sweep (no identifiers retained)");

    // salt sensitivity: the same MAC hashes differently under a different per-boot salt,
    // so the in-RAM hash is not a stable cross-boot identifier.
    observe_reset_ephemeral(0x11111111u); uint32_t h1 = observe_hash_mac(A);
    observe_reset_ephemeral(0x22222222u); uint32_t h2 = observe_hash_mac(A);
    ST_CHECK(h1 != h2, "salt makes the dedup hash non-stable across boots");
}

static void test_rf_model_nvs(void)
{
    rf_model_t m; rf_model_reset(&m);
    rf_model_observe(&m, 0x004C, -50, 0, 150);
    rf_model_observe(&m, 0x0075, -60, 3, 900);
    rf_model_end_sweep(&m, 3, 60000, 3);

    ST_CHECK(rf_model_save_nvs(&m) == 0, "rf_model saves to NVS");
    rf_model_t r; memset(&r, 0xAA, sizeof(r));
    ST_CHECK(rf_model_load_nvs(&r) == 0, "rf_model loads from NVS");
    ST_CHECK(memcmp(&m, &r, sizeof(m)) == 0, "rf_model NVS round-trips byte-exact");
}

static void test_vendor_mfg_builder(void)
{
    uint8_t pay[31], len = 0;
    ST_CHECK(template_build_vendor_mfg(0x0040, pay, &len) == 0 && len > 0 && len <= 31,
             "generic vendor-mfg builds for an arbitrary company id");
    bool found = false;                         // company id present in a 0xFF mfg AD, little-endian
    for (int i = 0; i + 1 < len; ) {
        uint8_t adlen = pay[i], adtype = pay[i+1];
        if (adtype == 0xFF && adlen >= 3 && pay[i+2] == 0x40 && pay[i+3] == 0x00) found = true;
        i += adlen + 1;
    }
    ST_CHECK(found, "generic vendor-mfg carries the requested company id");
}

static void test_generate(void)
{
    rf_model_t m; rf_model_reset(&m);
    // 70% company 0x0040 @ ~150 ms, 30% Samsung 0x0075 @ ~900 ms
    for (int i=0;i<70;i++) rf_model_observe(&m, 0x0040, -55, 0, 150);
    for (int i=0;i<30;i++) rf_model_observe(&m, 0x0075, -65, 0, 900);
    rf_model_end_sweep(&m, 6, 60000, 6);

    static identity_t roster[64];
    size_t built = generate_roster(&m, roster, 64);
    ST_CHECK(built == 64, "every generated identity has a payload");
    int c40=0, c75=0; bool budget=true, mac=true, nopop=true, arch=true;
    for (size_t i=0;i<64;i++){
        if (roster[i].company_id==0x0040) c40++;
        if (roster[i].company_id==0x0075) c75++;
        if (roster[i].payload_len==0 || roster[i].payload_len>31) budget=false;
        if ((roster[i].addr[5]&0xc0)!=0xc0) mac=false;
        if (has_apple_popup_subtype(roster[i].payload, roster[i].payload_len)) nopop=false;
        if (roster[i].archetype_idx >= templates_count()) arch=false;
    }
    ST_CHECK(budget, "generated payloads fit 31 bytes");
    ST_CHECK(mac, "generated MACs are random-static");
    ST_CHECK(nopop, "generated roster: no forbidden Apple subtype");
    ST_CHECK(arch, "generated identities carry a valid archetype index");
    ST_CHECK(c40 > c75, "generated vendor mix tracks the observed mix (0x0040 dominant)");

    uint8_t at = generate_active_target(&m);   // pop=6 -> Ward 9 / Shade 7, clamped
    ST_CHECK(at >= GEN_FLOOR_TEST_MIN && at <= CHURN_ACTIVE_SET, "active target clamped to persona range");

    rf_model_t empty; rf_model_reset(&empty);
    ST_CHECK(empty.total_obs < GEN_MIN_OBS, "empty model is below the generate threshold (caller falls back)");
}

static void test_roster_generate_path(void)
{
    rf_model_t m; rf_model_reset(&m);
    for (int i=0;i<80;i++) rf_model_observe(&m, 0x0075, -60, 0, 900);
    rf_model_end_sweep(&m, 5, 60000, 5);
    rf_model_save_nvs(&m);

    roster_init();
    bool ok=true; for (size_t i=0;i<CHURN_ROSTER_SIZE;i++){
        identity_t *id=roster_at(i);
        if (id->payload_len==0 || id->payload_len>31) ok=false;
        if (has_apple_popup_subtype(id->payload,id->payload_len)) ok=false;
        if (id->archetype_idx>=templates_count()) ok=false;
    }
    ST_CHECK(ok, "roster_init generate path: all payloads valid, Law-3-clean, valid archetype");
}

int churn_selftest_run(void)
{
    s_total = 0; s_fail = 0; s_first_fail = NULL;

    // --- roster ---
    roster_init();
    ST_CHECK(roster_count_in_state(ID_IDLE) == CHURN_ROSTER_SIZE,
             "roster_init: all identities IDLE");
    bool macs_ok = true, payload_ok = true;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = roster_at(i);
        if ((id->addr[5] & 0xc0) != 0xc0) macs_ok = false;     // random-static
        if (id->payload_len == 0) payload_ok = false;
    }
    ST_CHECK(macs_ok, "roster: every MAC is random-static (top 2 bits set)");
    ST_CHECK(payload_ok, "roster: every identity has a non-empty payload");

    identity_t *c = roster_promote_candidate(0);
    ST_CHECK(c != NULL, "promote_candidate returns an identity");

    // --- churn lifecycle ---
    test_churn_lifecycle();
    test_cooldown();
    test_timeslice();

    // --- M4 templates ---
    test_templates();
    test_ibeacon();
    test_eddystone();
    test_tracker();
    test_roster_payloads();
    test_rf_model();
    test_observe_dedup();
    test_rf_model_nvs();
    test_vendor_mfg_builder();
    test_generate();
    test_roster_generate_path();

    ESP_LOGW(TAG, "SELFTEST: %s (%d/%d)", s_fail ? "FAIL" : "PASS",
             s_total - s_fail, s_total);
    return s_fail;
}
