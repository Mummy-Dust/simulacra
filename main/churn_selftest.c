#include <stdbool.h>
#include <string.h>
#include "churn_selftest.h"
#include "roster.h"
#include "churn.h"
#include "templates.h"
#include "esp_log.h"

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

    // Collect the identities placed on radios across two consecutive slices.
    const identity_t *seen[CHURN_HW_INSTANCES * 2]; int n = 0;
    churn_tick(CHURN_SLICE_MS);            // slice phase 1
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) seen[n++] = s_rec[i];
    churn_tick(2 * CHURN_SLICE_MS);        // slice phase 2
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) seen[n++] = s_rec[i];

    // With ACTIVE_SET=8, HW=4, every active identity should appear within 2 slices.
    int covered = 0;
    for (int s = 0; s < CHURN_ACTIVE_SET; s++) {
        const identity_t *a = churn_active_at(s);
        for (int j = 0; j < n; j++) if (seen[j] == a) { covered++; break; }
    }
    ST_CHECK(covered == CHURN_ACTIVE_SET,
             "every active identity is on-air within 2 slices");
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

    ESP_LOGW(TAG, "SELFTEST: %s (%d/%d)", s_fail ? "FAIL" : "PASS",
             s_total - s_fail, s_total);
    return s_fail;
}
