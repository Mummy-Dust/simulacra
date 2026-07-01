#include <stdbool.h>
#include <string.h>
#include "churn_selftest.h"
#include "roster.h"
#include "churn.h"
#include "templates.h"
#include "rf_model.h"
#include "observe.h"
#include "generate.h"
#include "probe.h"
#include "drift.h"
#include "coexist.h"
#include "detect.h"
#include "webui.h"
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

static void test_probe_frame(void)
{
    const uint8_t mac[6] = {0x12,0x34,0x56,0x78,0x9a,0xbc};
    uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
    ST_CHECK(probe_build_request(mac, 6, f, &n) == 0 && n >= 24 && n <= PROBE_FRAME_MAX, "probe frame builds, valid length");
    ST_CHECK(f[0] == 0x40 && f[1] == 0x00, "frame control = probe request");
    bool da=true, bssid=true, sa=true;
    for (int i=0;i<6;i++){ if (f[4+i]!=0xff) da=false; if (f[16+i]!=0xff) bssid=false; if (f[10+i]!=mac[i]) sa=false; }
    ST_CHECK(da && bssid, "DA + BSSID are broadcast");
    ST_CHECK(sa, "SA = supplied MAC");
    ST_CHECK(f[24]==0x00 && f[25]==0x00, "SSID IE is wildcard (len 0) -- Law 3");
    ST_CHECK(f[26]==0x01, "Supported Rates IE present");
    bool directed=false;
    for (size_t i=24; i+1 < n; ) { uint8_t id=f[i], ln=f[i+1]; if (id==0x00 && ln!=0) directed=true; i += 2+ln; }
    ST_CHECK(!directed, "no directed (named-SSID) probe ever emitted");

    uint8_t a[6], b[6]; probe_random_mac(a); probe_random_mac(b);
    ST_CHECK((a[0]&0x03)==0x02, "random MAC is locally-administered + unicast");
    ST_CHECK(memcmp(a,b,6)!=0, "random MAC varies across calls");
}

static void test_accel_rotation(void)
{
    roster_init();
    churn_set_apply(noop_apply);
    churn_set_accel(1.0f);
    churn_init(0);

    // With 3x accel, every freshly-promoted dwell is bounded by ~MAX/3.
    churn_set_accel(3.0f);
    uint32_t accel_max = 0, now = 10000;
    for (int k = 0; k < 20; k++) {
        ((identity_t *)churn_active_at(0))->active_until_ms = now;     // force-retire slot 0
        churn_tick(now);
        const identity_t *p = churn_active_at(0);
        if (p) { uint32_t d = p->active_until_ms - now; if (d > accel_max) accel_max = d; }
        now += 1000;
    }
    ST_CHECK(accel_max <= CHURN_DWELL_MAX_MS / 3 + 1, "accel(3.0) bounds dwell to ~MAX/3");
    ST_CHECK(accel_max >= CHURN_DWELL_MIN_MS / 3,     "accel(3.0) dwell still >= MIN/3");

    // Decay back to 1.0 restores the full dwell range (some sample exceeds MAX/3).
    churn_set_accel(1.0f);
    uint32_t base_max = 0;
    for (int k = 0; k < 20; k++) {
        ((identity_t *)churn_active_at(0))->active_until_ms = now;
        churn_tick(now);
        const identity_t *p = churn_active_at(0);
        if (p) { uint32_t d = p->active_until_ms - now; if (d > base_max) base_max = d; }
        now += 1000;
    }
    ST_CHECK(base_max > CHURN_DWELL_MAX_MS / 3, "accel decays to 1.0 -> full dwell range restored");
}

static void test_drift(void)
{
    rf_model_t a; rf_model_reset(&a);                  // 100% Apple
    for (int i=0;i<100;i++) rf_model_observe(&a, 0x004C, -50, 0, -1);
    rf_model_end_sweep(&a, 5, 60000, 5);
    ST_CHECK(drift_score(&a, &a) < 0.01f, "drift: identical models score ~0");

    rf_model_t b; rf_model_reset(&b);                  // 100% Samsung (disjoint mix)
    for (int i=0;i<100;i++) rf_model_observe(&b, 0x0075, -50, 0, -1);
    rf_model_end_sweep(&b, 5, 60000, 5);
    float far = drift_score(&a, &b);
    ST_CHECK(far > 0.6f, "drift: disjoint vendor mixes score high");
    ST_CHECK(drift_exceeds(far, 0.5f), "drift_exceeds true above threshold");

    rf_model_t c; rf_model_reset(&c);                  // 70/30 partial overlap with a
    for (int i=0;i<70;i++) rf_model_observe(&c, 0x004C, -50, 0, -1);
    for (int i=0;i<30;i++) rf_model_observe(&c, 0x0075, -50, 0, -1);
    rf_model_end_sweep(&c, 5, 60000, 5);
    float mid = drift_score(&a, &c);
    ST_CHECK(mid > 0.01f && mid < far, "drift: partial overlap is monotonic with mix distance");
    ST_CHECK(!drift_exceeds(0.1f, 0.5f), "drift_exceeds false below threshold");
}

static void test_scheduler_budget(void)
{
    const coexist_persona_t *p = coexist_persona();
    ST_CHECK(p->wifi_period_ms > 0 && p->reprofile_period_ms > 0, "persona has positive cadences");
    ST_CHECK(p->reprofile_period_ms > p->wifi_period_ms, "re-profile is rarer than Wi-Fi bursts");

    // Drive a 250 ms tick (= CHURN_TICK_MS) over one hour; count fired slots.
    uint32_t lw = 0, lr = 0; int wifi = 0, repro = 0;
    for (uint32_t t = 0; t <= 3600000u; t += 250) {
        coexist_due_t d = coexist_due(p, t, &lw, &lr);
        if (d.fire_wifi)      wifi++;
        if (d.fire_reprofile) repro++;
    }
    int exp_wifi  = (int)(3600000u / p->wifi_period_ms);
    int exp_repro = (int)(3600000u / p->reprofile_period_ms);
    ST_CHECK(wifi  >= exp_wifi  - 1 && wifi  <= exp_wifi  + 1, "Wi-Fi bursts fire at the persona cadence");
    ST_CHECK(repro >= exp_repro - 1 && repro <= exp_repro + 1, "re-profile fires at the persona cadence");

#if CONFIG_IDF_TARGET_ESP32C5
    ST_CHECK(p->use_5g,                  "Ward (C5) schedules 5 GHz excursions");
    ST_CHECK(p->drift_threshold >= 1.0f, "Ward drift threshold effectively off (stationary)");
#else
    ST_CHECK(!p->use_5g,                 "Shade (C6) is 2.4 GHz only");
    ST_CHECK(p->drift_threshold < 1.0f,  "Shade drift threshold active (anti-entourage)");
#endif
}

static void test_detect_epochs(void)
{
    detect_reset();
    // Same device (hash 0xAAAA), 2 sightings each across 3 distinct epochs -> CONFIRM on the 3rd.
    detect_result_t r = DETECT_NONE;
    for (uint16_t e = 1; e <= 3; e++) {
        detect_on_epoch_change(e);
        r = detect_observe(0xAAAA, -50, 0x0075, e);   // sighting 1: credit pending
        detect_result_t r2 = detect_observe(0xAAAA, -50, 0x0075, e); // sighting 2: credit
        if (e < 3) ST_CHECK(r == DETECT_NONE && r2 == DETECT_NONE, "no confirm before 3 epochs");
        else       ST_CHECK(r2 == DETECT_CONFIRM, "confirm on the 3rd distinct epoch");
    }
    ST_CHECK(detect_threat_count() == 1, "one confirmed threat recorded");
    detect_threat_t t;
    ST_CHECK(detect_threat_at(0, &t) && t.hash == 0xAAAA && t.epochs >= DETECT_EPOCH_STRIKES,
             "threat record carries the hash + >=3 epochs");
    // A subsequent sighting of a confirmed threat returns KNOWN, not CONFIRM.
    ST_CHECK(detect_observe(0xAAAA, -40, 0x0075, 4) == DETECT_KNOWN, "confirmed threat -> KNOWN");
}

static void test_detect_presence(void)
{
    detect_reset();
    // A single sighting per epoch never earns a credit -> never confirms.
    for (uint16_t e = 1; e <= 6; e++) {
        detect_on_epoch_change(e);
        ST_CHECK(detect_observe(0xBBBB, -50, 0, e) == DETECT_NONE, "single-sighting epoch earns no credit");
    }
    ST_CHECK(detect_threat_count() == 0, "no confirm without meaningful presence");
}

static void test_detect_no_false_confirm(void)
{
    detect_reset();
    // Many one-shot devices, one sighting each in one epoch: a drive-by pattern, never confirms.
    detect_on_epoch_change(1);
    for (uint32_t h = 0; h < 20; h++) ST_CHECK(detect_observe(0x1000 + h, -60, 0, 1) == DETECT_NONE,
                                               "drive-by device does not confirm");
    // Two credited sightings but only in ONE epoch: still below the strike bar.
    detect_observe(0x2222, -50, 0, 1); detect_observe(0x2222, -50, 0, 1);
    ST_CHECK(detect_threat_count() == 0, "presence in a single epoch is not a follower");
}

static void test_detect_ageout(void)
{
    detect_reset();
    // Device seen once at epoch 1, then absent. After AGE_OUT_EPOCHS it must be forgotten,
    // so a later re-appearance starts fresh (never accumulates a stale credit).
    detect_on_epoch_change(1);
    detect_observe(0xCAFE, -50, 0, 1); detect_observe(0xCAFE, -50, 0, 1);   // credited epoch 1
    for (uint16_t e = 2; e <= 1 + DETECT_AGE_OUT_EPOCHS; e++) detect_on_epoch_change(e);
    // Now re-appear: because it was aged out, it must take a full fresh run to confirm.
    uint16_t e0 = 2 + DETECT_AGE_OUT_EPOCHS;
    detect_result_t r = DETECT_NONE;
    for (uint16_t e = e0; e < e0 + 2; e++) { detect_on_epoch_change(e);
        detect_observe(0xCAFE, -50, 0, e); r = detect_observe(0xCAFE, -50, 0, e); }
    ST_CHECK(r == DETECT_NONE, "aged-out candidate does not carry its old epoch credit");

    // LRU bound: pushing many distinct devices never overflows the table.
    detect_reset();
    for (uint32_t h = 0; h < DETECT_MAX_CANDIDATES * 3u; h++) detect_observe(0x8000 + h, -60, 0, 1);
    ST_CHECK(detect_threat_count() == 0, "LRU churn of distinct one-shot devices confirms nothing");
}

static void test_detect_locate_throttle(void)
{
    // Elapsed >= MIN_MS -> due regardless of RSSI (periodic heartbeat).
    ST_CHECK(detect_locate_due(-50, -50, DETECT_LOCATE_MIN_MS, 0), "locate due after min interval");
    // Below the hard floor -> never due, even on a big RSSI swing (kills per-packet spam).
    ST_CHECK(!detect_locate_due(-40, -80, 500, 0), "big RSSI move below floor is rate-capped");
    // Small RSSI move above the floor but within MIN -> not due.
    ST_CHECK(!detect_locate_due(-50, -47, DETECT_LOCATE_FLOOR_MS, 0), "small RSSI move above floor is throttled");
    // Big RSSI move above the floor (but within MIN) -> due (getting-warmer).
    ST_CHECK(detect_locate_due(-40, -50, DETECT_LOCATE_FLOOR_MS, 0), "RSSI delta above floor emits");
    ST_CHECK(detect_locate_due(-60, -50, DETECT_LOCATE_FLOOR_MS, 0), "RSSI delta is symmetric (getting colder counts)");
}

static void test_detect_self_exclude(void)
{
    const uint8_t set[3][6] = {
        {0x01,0x02,0x03,0x04,0x05,0xC0},
        {0x11,0x12,0x13,0x14,0x15,0xC1},
        {0x21,0x22,0x23,0x24,0x25,0xC2},
    };
    const uint8_t self_mac[6] = {0x11,0x12,0x13,0x14,0x15,0xC1};
    const uint8_t other[6]    = {0xAA,0xBB,0xCC,0xDD,0xEE,0xC3};
    ST_CHECK(detect_mac_in_set(self_mac, set, 3), "our own active MAC is recognized (excluded)");
    ST_CHECK(!detect_mac_in_set(other, set, 3),   "a foreign MAC is not excluded");
    ST_CHECK(!detect_mac_in_set(self_mac, set, 0), "empty set excludes nothing");
}

static void test_detect_nvs(void)
{
    detect_reset();
    // Confirm a threat, then round-trip the threat table through NVS.
    for (uint16_t e = 1; e <= 3; e++) { detect_on_epoch_change(e);
        detect_observe(0xD00D, -44, 0x004C, e); detect_observe(0xD00D, -44, 0x004C, e); }
    ST_CHECK(detect_threat_count() == 1, "threat confirmed before persist");

    // Pending-confirm drains exactly once.
    detect_threat_t p;
    ST_CHECK(detect_drain_pending(&p) && p.hash == 0xD00D, "pending confirm drains the new threat");
    ST_CHECK(!detect_drain_pending(&p), "pending flag clears after draining");

    ST_CHECK(detect_save_nvs() == 0, "threats save to NVS");
    detect_reset();
    ST_CHECK(detect_threat_count() == 0, "reset clears RAM threats");
    ST_CHECK(detect_load_nvs() == 0, "threats load from NVS");
    ST_CHECK(detect_threat_count() == 1, "one threat restored");
    detect_threat_t t;
    ST_CHECK(detect_threat_at(0, &t) && t.hash == 0xD00D && t.vendor == 0x004C && t.epochs >= 3,
             "restored threat round-trips hash/vendor/epochs");

    // Salt persists (stable across a load), so cross-boot hashes are stable by design.
    uint32_t s1 = detect_load_salt();
    uint32_t s2 = detect_load_salt();
    ST_CHECK(s1 == s2 && s1 != 0, "per-install salt is stable and non-zero");
}

static void test_churn_pause(void)
{
    roster_init();
    churn_set_apply(noop_apply);
    churn_set_paused(false);
    churn_init(0);
    // Force slot 0 to expire, but pause first: a paused tick must NOT rotate it.
    const identity_t *before = churn_active_at(0);
    ((identity_t *)before)->active_until_ms = 1000;
    churn_set_paused(true);
    churn_tick(2000);
    ST_CHECK(churn_active_at(0) == before, "paused churn does not rotate identities");
    ST_CHECK(churn_paused(), "pause flag reads back true");
    // Resume: the expired identity now gets replaced.
    churn_set_paused(false);
    churn_tick(3000);
    ST_CHECK(churn_active_at(0) != before, "resumed churn rotates the expired identity");
}

static void test_detect_clear(void)
{
    detect_reset();
    for (uint16_t e = 1; e <= 3; e++) { detect_on_epoch_change(e);
        detect_observe(0xFEED, -44, 0x004C, e); detect_observe(0xFEED, -44, 0x004C, e); }
    ST_CHECK(detect_threat_count() == 1, "threat confirmed before clear");
    detect_save_nvs();
    detect_clear_threats();
    ST_CHECK(detect_threat_count() == 0, "clear empties the RAM threat table");
    detect_reset();
    detect_load_nvs();   // blob was erased -> returns non-zero; the point is nothing is restored
    ST_CHECK(detect_threat_count() == 0, "cleared threats do not come back from NVS");
}

static void test_webui_json(void)
{
    webui_status_t st = {0};
    st.uptime_s = 123; st.decoy_paused = false; st.wifi_config_mode = true;
    st.active_devices = 5; st.roster_size = 64; st.probes_sent = 4096;
    st.epoch = 7; st.pop_ewma = 12; st.total_obs = 999; st.active_target = 9;
    st.threat_count = 1;
    st.threats[0].hash = 0xD00D; st.threats[0].vendor = 0x004C;
    st.threats[0].best_rssi = -44; st.threats[0].epochs = 3;
    st.threats[0].first_epoch = 2; st.threats[0].last_epoch = 7;

    char buf[1024];
    int n = webui_build_status_json(buf, sizeof(buf), &st);
    ST_CHECK(n > 0, "status json serializes");
    ST_CHECK(strstr(buf, "\"uptime_s\":123") != NULL, "json carries uptime");
    ST_CHECK(strstr(buf, "\"active_devices\":5") != NULL, "json carries active count");
    ST_CHECK(strstr(buf, "\"probes_sent\":4096") != NULL, "json carries probe count");
    ST_CHECK(strstr(buf, "\"pop_ewma\":12") != NULL, "json carries population aggregate");
    ST_CHECK(strstr(buf, "\"threat_count\":1") != NULL, "json carries threat count");
    ST_CHECK(strstr(buf, "d00d") != NULL || strstr(buf, "D00D") != NULL,
             "json carries the threat hash");
    ST_CHECK(strstr(buf, ":") != NULL && buf[0] == '{', "json is an object");

    // Truncation: an undersized buffer must fail cleanly (no overrun, returns -1).
    char tiny[16];
    ST_CHECK(webui_build_status_json(tiny, sizeof(tiny), &st) == -1,
             "undersized buffer reports truncation");
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
    test_probe_frame();
    test_drift();
    test_accel_rotation();
    test_churn_pause();
    test_scheduler_budget();
    test_detect_epochs();
    test_detect_presence();
    test_detect_no_false_confirm();
    test_detect_ageout();
    test_detect_locate_throttle();
    test_detect_self_exclude();
    test_detect_nvs();
    test_detect_clear();
    test_webui_json();

    ESP_LOGW(TAG, "SELFTEST: %s (%d/%d)", s_fail ? "FAIL" : "PASS",
             s_total - s_fail, s_total);
    return s_fail;
}
