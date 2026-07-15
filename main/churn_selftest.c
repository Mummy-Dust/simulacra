#include <stdbool.h>
#include <string.h>
#include "churn_selftest.h"
#include "roster.h"
#include "churn.h"
#include "ble_devices.h"
#include "settings.h"
#include "templates.h"
#include "rf_model.h"
#include "observe.h"
#include "generate.h"
#include "probe.h"
#include "drift.h"
#include "coexist.h"
#include "detect.h"
#include "webui.h"
#include "radar_geom.h"
#include "radar_ui.h"
#include "radar_wire.h"
#include "radar_key.h"
#include "config_wire.h"
#include "enroll_wire.h"
#include "tweetnacl.h"
#include "esp_now_link.h"
#include "law3.h"
#include "learn.h"
#include "learn_wire.h"
#include "learn_db.h"
#include "threat_sig.h"
#include "sig_match.h"
#include "sig_db.h"
#include "sig_wire.h"
#include "sig_seed.h"
#include "sig_class_name.h"
#include "sig_store.h"
#include "threat_escalation.h"
#include "fleet.h"
#include "fleet_key.h"
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

static const identity_t *s_rec[CHURN_HW_INSTANCES];
static int s_apply_calls;
static int rec_apply(uint8_t instance, const identity_t *id)
{ if (instance < CHURN_HW_INSTANCES) s_rec[instance] = id; s_apply_calls++; return 0; }

// Fixed population for the churn self-tests: > CHURN_HW_INSTANCES so time-slicing engages.
#define CHURN_ST_N 8

// Milestone A: churn is now a presenter over ble_devices (lifecycle/rotation live there).
// This just checks the minimal new-contract wiring: population is visible through churn's
// accessors, and a tick drives at least one apply.
static void test_churn_present(void)
{
    roster_init();
    ble_devices_init(CHURN_ST_N, 0);
    memset(s_rec, 0, sizeof(s_rec));
    s_apply_calls = 0;
    churn_set_apply(rec_apply);
    churn_init(0);

    ST_CHECK(churn_active_count() == CHURN_ST_N, "active count mirrors ble_devices population");
    churn_tick(CHURN_SLICE_MS);
    ST_CHECK(churn_active_at(0) != NULL, "slot 0 resolves to a live identity");
    ST_CHECK(s_apply_calls > 0, "a tick drives at least one apply");
}

static void test_timeslice(void)
{
    roster_init();
    ble_devices_init(CHURN_ST_N, 0);
    memset(s_rec, 0, sizeof(s_rec));
    s_apply_calls = 0;
    churn_set_apply(rec_apply);
    churn_init(0);

    int n_pop = (int)churn_active_count();
    ST_CHECK(n_pop == CHURN_ST_N, "active count mirrors ble_devices population");
    bool all_present = true;
    for (int i = 0; i < n_pop; i++) if (!churn_active_at((size_t)i)) all_present = false;
    ST_CHECK(all_present, "every active slot (0..N) resolves to a non-NULL identity");

    // Collect the identities placed on radios across enough slices to cover the whole population.
    // (Population is well below rotation/lifetime timescales -- device pointers are stable here.)
    int slices = (n_pop + CHURN_HW_INSTANCES - 1) / CHURN_HW_INSTANCES;
    const identity_t *seen[CHURN_ST_N * 4]; int n = 0;
    for (int sl = 1; sl <= slices; sl++) {
        churn_tick((uint32_t)sl * CHURN_SLICE_MS);
        for (int i = 0; i < CHURN_HW_INSTANCES; i++) seen[n++] = s_rec[i];
    }

    // Every device should appear within ceil(N / HW) slices.
    int covered = 0;
    for (int s = 0; s < n_pop; s++) {
        const identity_t *a = churn_active_at((size_t)s);
        for (int j = 0; j < n; j++) if (seen[j] == a) { covered++; break; }
    }
    ST_CHECK(covered == n_pop, "every device is on-air within ceil(N/HW) slices");
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
// has_apple_popup_subtype + law3_forbidden now live in law3.{h,c} (shared with learn.c).
static void test_law3(void)
{
    // Apple Continuity nearby-action (0x0F) mfg-data: 4C 00 0F .. -> forbidden
    uint8_t nearby[] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    ST_CHECK(law3_forbidden(nearby, sizeof nearby), "law3: Apple nearby-action forbidden");

    // iBeacon (0x02) is allowed
    uint8_t ibeacon[] = { 0x02,0x01,0x06, 0x1A,0xFF,0x4C,0x00,0x02,0x15,
                          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0, 0,0, 0xC5 };
    ST_CHECK(!law3_forbidden(ibeacon, sizeof ibeacon), "law3: iBeacon allowed");

    // Microsoft Swift Pair: mfg len 5 (type + company 0006 + subtype) -> forbidden
    uint8_t swift[] = { 0x05,0xFF,0x06,0x00,0x03,0x00 };
    ST_CHECK(law3_forbidden(swift, sizeof swift), "law3: Swift Pair forbidden");

    // Google Fast Pair: svc-data len 5 (type + UUID 0xFE2C + payload) -> forbidden
    uint8_t fastpair[] = { 0x03,0x03,0x2C,0xFE, 0x05,0x16,0x2C,0xFE,0x00,0x00 };
    ST_CHECK(law3_forbidden(fastpair, sizeof fastpair), "law3: Fast Pair forbidden");

    // Plain vendor-mfg (Samsung 0x0075) -> allowed
    uint8_t vendor[] = { 0x06,0xFF,0x75,0x00,0x01,0x02,0x03 };
    ST_CHECK(!law3_forbidden(vendor, sizeof vendor), "law3: plain vendor allowed");
}

// ---- self-learning templates (learn.c) ----

static void mk_shape(learned_template_t *t, uint16_t company)
{
    memset(t, 0, sizeof(*t));
    t->ad_len = 6; t->ad[0]=0x05; t->ad[1]=0xFF;
    t->ad[2]=(uint8_t)company; t->ad[3]=(uint8_t)(company>>8); t->ad[4]=1; t->ad[5]=2;
    t->company_id = company; t->family = FMT_VENDOR_MFG;
    t->itvl_min_ms = 100; t->itvl_max_ms = 200;
    t->shape_hash = learn_shape_hash(t);
}

static void test_learn_strip(void)
{
    learned_template_t t;
    // Named Samsung earbud: flags + mfg(0075 AB CD) + complete-name "Buds Pro"
    uint8_t named[] = { 0x02,0x01,0x06,
                        0x05,0xFF,0x75,0x00,0xAB,0xCD,
                        0x09,0x09,'B','u','d','s',' ','P','r','o' };
    ST_CHECK(learn_strip(named, sizeof named, 0x0075, &t), "strip: named earbud accepted");
    ST_CHECK(t.company_id == 0x0075, "strip: company_id preserved");
    ST_CHECK(t.name_len == 8 && t.name_off > 0, "strip: name region captured");
    bool name_scrubbed = true;
    for (uint8_t b = t.name_off; b < t.name_off + t.name_len; b++)
        if (t.ad[b] != 0) name_scrubbed = false;
    ST_CHECK(name_scrubbed, "strip: local-name value scrubbed from skeleton");
    ST_CHECK((t.rand_mask & (1u << 7)) && (t.rand_mask & (1u << 8)),
             "strip: mfg blob bytes masked");
    ST_CHECK(!(t.rand_mask & (1u << 5)) && !(t.rand_mask & (1u << 6)),
             "strip: company id not masked");
    uint8_t nearby[] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    ST_CHECK(!learn_strip(nearby, sizeof nearby, 0x004C, &t), "strip: forbidden rejected");

    // Zero-padded AD (common on real reports): trailing zeros are end-of-AD, not a reject.
    uint8_t padded[] = { 0x02,0x01,0x06, 0x05,0xFF,0x75,0x00,0xAB,0xCD, 0x00,0x00,0x00 };
    ST_CHECK(learn_strip(padded, sizeof padded, 0x0075, &t), "strip: zero-padded AD accepted");
}

static bool has_digit_run(const uint8_t *b, size_t from, size_t to, int n)
{
    int run = 0;
    for (size_t i = from; i < to && i < 31; i++) {
        if (b[i] >= '0' && b[i] <= '9') { if (++run >= n) return true; } else run = 0;
    }
    return false;
}

static void test_learn_render(void)
{
    // Named Samsung earbud with an 11-char name -- the length that made the old digit-pad tell
    // ("Beat1701876"). Name is the last AD element (name_off + name_len == ad_len).
    uint8_t named[] = { 0x02,0x01,0x06,
                        0x05,0xFF,0x75,0x00,0xAB,0xCD,
                        0x0C,0x09,'G','a','l','a','x','y',' ','B','u','d','s' };
    learned_template_t t;
    ST_CHECK(learn_strip(named, sizeof named, 0x0075, &t), "render: strip ok");
    t.itvl_min_ms = 100; t.itvl_max_ms = 200;
    ST_CHECK(t.name_len == 11, "render: 11-char name captured");

    learned_template_t t2 = t; t2.company_id = 0x009E; t2.ad[2] = 0x9E;
    t2.shape_hash = learn_shape_hash(&t2);
    ST_CHECK(learn_shape_hash(&t) == learn_shape_hash(&t),  "hash: stable");
    ST_CHECK(learn_shape_hash(&t) != learn_shape_hash(&t2), "hash: company changes it");

    uint8_t a[31]; uint8_t la; uint16_t ia;
    bool clean = true, budget = true, no_digits = true, printable = true;
    for (int k = 0; k < 24; k++) {                 // many renders: names/lengths vary, none digit-pads
        ST_CHECK(learn_render(&t, a, &la, &ia) == 0, "render ok");
        if (law3_forbidden(a, la)) clean = false;
        if (la > 31) budget = false;
        if (has_digit_run(a, t.name_off, la, 4)) no_digits = false;   // the "Beat1701876" tell
        for (uint8_t i = t.name_off; i < la; i++) if (a[i] < 0x20 || a[i] > 0x7E) printable = false;
    }
    ST_CHECK(clean, "render: Law-3 clean across renders");
    ST_CHECK(budget, "render: within 31-byte budget");
    ST_CHECK(printable, "render: name region is printable ASCII");
    ST_CHECK(no_digits, "render: no digit-run padding in the name (realism)");
    ST_CHECK(a[5] == 0x75 && a[6] == 0x00, "render: company id stable");
    ST_CHECK(ia >= 100 && ia <= 200, "render: interval in band");

    // Name NOT last (mfg data follows it): captured length must be preserved exactly, still no digits.
    uint8_t nlast[] = { 0x02,0x01,0x06,
                        0x0C,0x09,'G','a','l','a','x','y',' ','B','u','d','s',
                        0x05,0xFF,0x75,0x00,0xAB,0xCD };
    learned_template_t tn;
    ST_CHECK(learn_strip(nlast, sizeof nlast, 0x0075, &tn), "render(nlast): strip ok");
    tn.itvl_min_ms = 100; tn.itvl_max_ms = 200;
    uint8_t c[31]; uint8_t lc; uint16_t ic;
    ST_CHECK(learn_render(&tn, c, &lc, &ic) == 0, "render(nlast): ok");
    ST_CHECK(lc == tn.ad_len, "render(nlast): exact length preserved (name not last)");
    ST_CHECK(!has_digit_run(c, tn.name_off, tn.name_off + tn.name_len, 4), "render(nlast): no digit padding");
}

static void test_learn_store(void)
{
    learn_reset();
    ST_CHECK(learn_count() == 0, "store: empty after reset");

    learned_template_t a; mk_shape(&a, 0x0075);
    ST_CHECK(learn_store_add(&a, 1) && learn_count() == 1, "store: first add");
    ST_CHECK(learn_store_add(&a, 2) && learn_count() == 1, "store: dedup reinforces");
    ST_CHECK(learn_at(0)->reinforce_count >= 1, "store: reinforce_count bumped");

    learned_template_t b; mk_shape(&b, 0x009E);
    ST_CHECK(learn_store_add(&b, 2) && learn_count() == 2, "store: distinct shape added");

    learn_age_out(2 + LEARN_AGEOUT_SWEEPS + 1);
    ST_CHECK(learn_count() < 2, "store: stale entry aged out");
}

static void test_learn_pipeline(void)
{
    learn_reset();
    uint8_t buds[] = { 0x02,0x01,0x06, 0x05,0xFF,0x75,0x00,0xAB,0xCD };
    uint32_t H = 0xBEEF1234;

    for (int i = 0; i < LEARN_MIN_SIGHTINGS - 1; i++)
        learn_offer(H, buds, sizeof buds, 0x0075, 100 * (i + 1));
    ST_CHECK(learn_count() == 0, "pipeline: below K not learned");

    learn_offer(H, buds, sizeof buds, 0x0075, 100 * LEARN_MIN_SIGHTINGS);
    ST_CHECK(learn_count() == 1, "pipeline: reaching K promotes one template");

    uint8_t nearby[] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    for (int i = 0; i < LEARN_MIN_SIGHTINGS + 2; i++)
        learn_offer(0xF00D, nearby, sizeof nearby, 0x004C, 50 * (i + 1));
    ST_CHECK(learn_count() == 1, "pipeline: forbidden never promoted");

    learn_end_sweep(1);
    learn_offer(H, buds, sizeof buds, 0x0075, 10);
    ST_CHECK(learn_count() == 1, "pipeline: post-sweep single sighting no new promote");
}

static void test_learn_nvs(void)
{
    learn_reset();
    learned_template_t a; mk_shape(&a, 0x0075); learn_store_add(&a, 1);
    learned_template_t b; mk_shape(&b, 0x009E); learn_store_add(&b, 1);
    ST_CHECK(learn_count() == 2, "learn nvs: two before save");
    ST_CHECK(learn_save_nvs() == 0, "learn nvs: save ok");

    learn_reset();
    ST_CHECK(learn_count() == 0, "learn nvs: reset clears RAM");
    ST_CHECK(learn_load_nvs() == 0, "learn nvs: load ok");
    ST_CHECK(learn_count() == 2, "learn nvs: two restored");
    ST_CHECK(learn_at(0)->company_id == 0x0075, "learn nvs: entry round-trips");
}

static void test_learn_generate(void)
{
    learn_reset();
    uint8_t buds[] = { 0x02,0x01,0x06, 0x05,0xFF,0x75,0x00,0xAB,0xCD };
    for (int i = 0; i < LEARN_MIN_SIGHTINGS; i++)
        learn_offer(0xABCD, buds, sizeof buds, 0x0075, 100 * (i + 1));
    ST_CHECK(learn_count() == 1, "gen: one learned shape seeded");

    rf_model_t m; rf_model_reset(&m);
    for (int i = 0; i < 60; i++) rf_model_observe(&m, 0x0075, -50, 0, 150);
    rf_model_end_sweep(&m, 5 /*distinct*/, 15000 /*window_ms*/, 5 /*arrivals*/);

    identity_t roster[8];
    size_t n = generate_roster(&m, roster, 8);
    ST_CHECK(n == 8, "gen: full roster built");
    bool clean = true, bound = true;
    for (size_t i = 0; i < 8; i++) {
        if (roster[i].payload_len == 0 || roster[i].payload_len > 31) clean = false;
        if (has_apple_popup_subtype(roster[i].payload, roster[i].payload_len)) clean = false;
        if (roster[i].archetype_idx >= templates_count() + learn_count()) bound = false;
    }
    ST_CHECK(clean, "gen: learned-inclusive roster payloads clean & in-budget");
    ST_CHECK(bound, "gen: archetype_idx within static+learned range");
    learn_reset();   // leave the global store empty for subsequent tests
}

static void test_learn_wire(void)
{
    // Build a 4-record set (distinct shapes).
    learned_template_t set[4];
    for (int i = 0; i < 4; i++) { mk_shape(&set[i], (uint16_t)(0x0070 + i)); }

    // Pack as two chunks: [0..3) and [3..4).
    uint8_t p0[218], p1[218]; size_t l0, l1;
    ST_CHECK(learn_wire_pack(p0, &l0, &set[0], 3, 1, 0, 2) == 0 && l0 <= 218, "wire: chunk0 packs");
    ST_CHECK(learn_wire_pack(p1, &l1, &set[3], 1, 1, 1, 2) == 0 && l1 <= 218, "wire: chunk1 packs");

    // Reassemble via merge into a fresh store; out-of-order + a duplicate chunk0 must be harmless.
    learned_template_t store[8]; size_t cnt = 0; learn_chunk_hdr_t h;
    learned_template_t rx[LEARN_WIRE_RECS_PER_CHUNK]; uint8_t n;
    ST_CHECK(learn_wire_unpack(p1, l1, rx, &n, &h) == 0 && n == 1 && h.chunk_count == 2, "wire: unpack c1");
    for (uint8_t i = 0; i < n; i++) learn_merge(store, &cnt, 8, &rx[i], 1);
    ST_CHECK(learn_wire_unpack(p0, l0, rx, &n, &h) == 0 && n == 3, "wire: unpack c0");
    for (uint8_t i = 0; i < n; i++) learn_merge(store, &cnt, 8, &rx[i], 1);
    ST_CHECK(learn_wire_unpack(p0, l0, rx, &n, &h) == 0, "wire: unpack dup c0");
    for (uint8_t i = 0; i < n; i++) learn_merge(store, &cnt, 8, &rx[i], 1);   // idempotent
    ST_CHECK(cnt == 4, "wire: reassembly complete + idempotent (4 records)");

    ST_CHECK(learn_wire_pack(p0, &l0, set, LEARN_WIRE_RECS_PER_CHUNK + 1, 1, 0, 1) != 0,
             "wire: over-capacity pack rejected");
}

static void test_learn_regate(void)
{
    learned_template_t ok; mk_shape(&ok, 0x0075);
    ST_CHECK(learn_regate(&ok), "regate: clean record accepted");

    // Tampered: inject an Apple nearby-action subtype into the skeleton.
    learned_template_t evil = ok;
    evil.ad_len = 9;
    uint8_t bad[9] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    memcpy(evil.ad, bad, 9);
    evil.shape_hash = learn_shape_hash(&evil);   // even with a valid hash, law3 must reject
    ST_CHECK(!learn_regate(&evil), "regate: forbidden subtype rejected");

    learned_template_t big = ok; big.ad_len = 32;
    ST_CHECK(!learn_regate(&big), "regate: over-budget ad_len rejected");

    learned_template_t liar = ok; liar.shape_hash ^= 0xFFFFFFFFu;
    ST_CHECK(!learn_regate(&liar), "regate: shape_hash mismatch rejected");
}

static void test_learn_merge_wire(void)
{
    learned_template_t store[4]; size_t cnt = 0;
    learned_template_t a; mk_shape(&a, 0x0075); a.reinforce_count = 5;

    ST_CHECK(learn_merge_wire(store, &cnt, 4, &a, 1) && cnt == 1, "merge_wire: first insert");
    ST_CHECK(store[0].reinforce_count == 5, "merge_wire: keeps incoming weight on insert");

    // Re-merging the same record must NOT inflate the weight (max, not increment) —
    // and a pure no-op duplicate reports "unchanged" so Vigil doesn't re-save the blob.
    ST_CHECK(!learn_merge_wire(store, &cnt, 4, &a, 2), "merge_wire: no-op dup reports unchanged");
    ST_CHECK(cnt == 1 && store[0].reinforce_count == 5, "merge_wire: dup keeps max (no inflation)");

    // A stronger copy raises it to the max — that IS a change.
    learned_template_t a2 = a; a2.reinforce_count = 9;
    ST_CHECK(learn_merge_wire(store, &cnt, 4, &a2, 3), "merge_wire: max raise reports changed");
    ST_CHECK(store[0].reinforce_count == 9, "merge_wire: max raises weight");

    // Widening the interval band is also a durable change.
    learned_template_t a3 = store[0]; a3.itvl_max_ms = store[0].itvl_max_ms + 100;
    ST_CHECK(learn_merge_wire(store, &cnt, 4, &a3, 4), "merge_wire: interval widen reports changed");
}

static void test_learn_snapshot_ingest(void)
{
    learn_reset();
    // Ingest two clean records as if received over the wire.
    learned_template_t a; mk_shape(&a, 0x0075); a.reinforce_count = 3;
    learned_template_t b; mk_shape(&b, 0x009E); b.reinforce_count = 1;
    ST_CHECK(learn_ingest_wire(&a) && learn_ingest_wire(&b), "ingest: two clean records accepted");
    ST_CHECK(learn_count() == 2, "ingest: store holds both");

    // A tampered record (forbidden subtype) is rejected by the re-gate.
    learned_template_t evil = a; uint8_t bad[9] = { 0x02,0x01,0x06, 0x05,0xFF,0x4C,0x00,0x0F,0x01 };
    memcpy(evil.ad, bad, 9); evil.ad_len = 9; evil.shape_hash = learn_shape_hash(&evil);
    ST_CHECK(!learn_ingest_wire(&evil), "ingest: forbidden record rejected");
    ST_CHECK(learn_count() == 2, "ingest: store unchanged after reject");

    // Snapshot exports what is held.
    learned_template_t out[8];
    size_t n = learn_snapshot(out, 8);
    ST_CHECK(n == 2, "snapshot: exports the stored count");
    learn_reset();
}

static void test_learn_db_key(void)
{
    uint8_t k1[32], k2[32];
    learn_db_derive_key(SIMULACRA_ESPNOW_KEY, k1);
    learn_db_derive_key(SIMULACRA_ESPNOW_KEY, k2);
    ST_CHECK(memcmp(k1, k2, 32) == 0, "db key: deterministic");
    ST_CHECK(memcmp(k1, SIMULACRA_ESPNOW_KEY, 32) != 0, "db key: distinct from session PSK");
    uint8_t other[32]; memcpy(other, SIMULACRA_ESPNOW_KEY, 32); other[0] ^= 0xFF;
    uint8_t k3[32]; learn_db_derive_key(other, k3);
    ST_CHECK(memcmp(k1, k3, 32) != 0, "db key: depends on PSK");
}

static void test_learn_db_blob(void)
{
    uint8_t key[32]; learn_db_derive_key(SIMULACRA_ESPNOW_KEY, key);
    learned_template_t in[3];
    for (int i = 0; i < 3; i++) { mk_shape(&in[i], (uint16_t)(0x0075 + i)); in[i].reinforce_count = (uint16_t)(i+1); }

    static uint8_t blob[sizeof(learn_db_hdr_t) + 3*sizeof(learned_template_t)]; size_t blen;
    ST_CHECK(learn_db_seal(blob, &blen, in, 3, key) == 0, "db blob: seal ok");
    ST_CHECK(blen == sizeof(learn_db_hdr_t) + 3*sizeof(learned_template_t), "db blob: length exact");

    learned_template_t out[3]; uint16_t n = 0;
    ST_CHECK(learn_db_open(blob, blen, out, &n, key) == 0 && n == 3, "db blob: open round-trips count");
    ST_CHECK(memcmp(in, out, 3*sizeof(learned_template_t)) == 0, "db blob: records identical");

    // Tamper one ciphertext byte -> tag must fail.
    static uint8_t bad[sizeof(blob)]; memcpy(bad, blob, blen);
    bad[sizeof(learn_db_hdr_t) + 4] ^= 0xFF;
    ST_CHECK(learn_db_open(bad, blen, out, &n, key) < 0, "db blob: tamper rejected");

    // Wrong key (foreign card) -> tag must fail.
    uint8_t wrong[32]; memcpy(wrong, key, 32); wrong[0] ^= 0xFF;
    ST_CHECK(learn_db_open(blob, blen, out, &n, wrong) < 0, "db blob: foreign key rejected");

    // Bad magic -> rejected.
    static uint8_t nomagic[sizeof(blob)]; memcpy(nomagic, blob, blen); nomagic[0] ^= 0xFF;
    ST_CHECK(learn_db_open(nomagic, blen, out, &n, key) < 0, "db blob: bad magic rejected");
}

static void test_learn_top_n(void)
{
    learned_template_t s[5];
    for (int i = 0; i < 5; i++) { mk_shape(&s[i], (uint16_t)(0x0070+i)); s[i].reinforce_count = (uint16_t)(i); }
    // reinforce_count = 0,1,2,3,4 ; top-3 should be the 4,3,2 set.
    learned_template_t out[3];
    size_t n = learn_top_n(s, 5, out, 3);
    ST_CHECK(n == 3, "top_n: returns n when count>=n");
    ST_CHECK(out[0].reinforce_count == 4 && out[1].reinforce_count == 3 && out[2].reinforce_count == 2,
             "top_n: strongest first");
    // n larger than count -> returns count.
    ST_CHECK(learn_top_n(s, 5, out, 3) == 3 && learn_top_n(s, 2, out, 3) == 2, "top_n: clamps to count");
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

// A MAC produced by make_random_addr_mixed/make_random_addr is a valid BLE random
// address whose top-2 bits select the subtype: 0xC0 static, 0x40 RPA, 0x00 NRPA.
static bool is_valid_random_top2(uint8_t last)
{
    uint8_t top2 = last & 0xc0;
    return top2 == 0xc0 || top2 == 0x40 || top2 == 0x00;
}

// Whole-roster structural + Law-3 guard: every identity must be a template-built,
// budget-fitting, non-popup payload on a valid random-subtype MAC.
static void test_roster_payloads(void)
{
    roster_init();
    bool macs_ok = true, payload_ok = true, no_popup = true, archetype_ok = true;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = roster_at(i);
        if (!is_valid_random_top2(id->addr[5])) macs_ok = false;
        if (id->payload_len == 0 || id->payload_len > 31) payload_ok = false;
        if (has_apple_popup_subtype(id->payload, id->payload_len)) no_popup = false;
        if (id->archetype_idx >= templates_count() + learn_count()) archetype_ok = false;
    }
    ST_CHECK(macs_ok, "roster: all MACs are a valid random subtype (static/RPA/NRPA)");
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
    // Dominant 0x0040 @ ~150 ms, small Samsung 0x0075 minority @ ~900 ms. The generation diversity
    // floor caps any single vendor at GEN_MAX_VENDOR_PCT and fills the overflow with varied templates,
    // so the dominant is pulled down toward the cap -- keep the minority small enough that the capped
    // dominant still clearly leads (otherwise the c40>c75 margin is thin and flaky).
    for (int i=0;i<70;i++) rf_model_observe(&m, 0x0040, -55, 0, 150);
    for (int i=0;i<10;i++) rf_model_observe(&m, 0x0075, -65, 0, 900);
    rf_model_end_sweep(&m, 6, 60000, 6);

    static identity_t roster[64];
    size_t built = generate_roster(&m, roster, 64);
    ST_CHECK(built == 64, "every generated identity has a payload");
    int c40=0, c75=0; bool budget=true, mac=true, nopop=true, arch=true;
    for (size_t i=0;i<64;i++){
        if (roster[i].company_id==0x0040) c40++;
        if (roster[i].company_id==0x0075) c75++;
        if (roster[i].payload_len==0 || roster[i].payload_len>31) budget=false;
        if (!is_valid_random_top2(roster[i].addr[5])) mac=false;
        if (has_apple_popup_subtype(roster[i].payload, roster[i].payload_len)) nopop=false;
        if (roster[i].archetype_idx >= templates_count() + learn_count()) arch=false;
    }
    ST_CHECK(budget, "generated payloads fit 31 bytes");
    ST_CHECK(mac, "generated MACs are a valid random subtype (static/RPA/NRPA)");
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
        if (id->archetype_idx>=templates_count() + learn_count()) ok=false;
    }
    ST_CHECK(ok, "roster_init generate path: all payloads valid, Law-3-clean, valid archetype");
}

static void test_probe_frame(void)
{
    const uint8_t mac[6] = {0x12,0x34,0x56,0x78,0x9a,0xbc};
    uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
    ST_CHECK(probe_build_request(mac, 6, ARCH_IPHONE, false, f, &n) == 0 && n >= 24 && n <= PROBE_FRAME_MAX, "probe frame builds, valid length");
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
    ST_CHECK(t.sessions_seen >= 1 && t.places_seen >= 1, "nvs: recurrence counters restored");

    // Salt persists (stable across a load), so cross-boot hashes are stable by design.
    uint32_t s1 = detect_load_salt();
    uint32_t s2 = detect_load_salt();
    ST_CHECK(s1 == s2 && s1 != 0, "per-install salt is stable and non-zero");
}

static void test_churn_pause(void)
{
    roster_init();
    ble_devices_init(CHURN_ST_N, 0);
    memset(s_rec, 0, sizeof(s_rec));
    s_apply_calls = 0;
    churn_set_apply(rec_apply);
    churn_set_paused(false);
    churn_init(0);

    // First slice always applies (fills the instances from the initial -1 occupancy).
    churn_tick(CHURN_SLICE_MS);
    ST_CHECK(s_apply_calls > 0, "initial tick applies the first occupant set");

    // Pause: further ticks -- even across many would-be slice boundaries -- must not apply.
    churn_set_paused(true);
    ST_CHECK(churn_paused(), "pause flag reads back true");
    int calls_before = s_apply_calls;
    for (uint32_t t = 2 * CHURN_SLICE_MS; t <= 10 * CHURN_SLICE_MS; t += CHURN_SLICE_MS)
        churn_tick(t);
    ST_CHECK(s_apply_calls == calls_before, "paused churn issues no apply calls");

    // Resume: the next slice boundary re-applies (time-slice advances the occupant set).
    churn_set_paused(false);
    churn_tick(11 * CHURN_SLICE_MS);
    ST_CHECK(s_apply_calls > calls_before, "resumed churn applies again");
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

static void test_radar_geometry(void)
{
    uint16_t near = radar_rssi_to_radius(-40, 10, 100);
    uint16_t mid  = radar_rssi_to_radius(-60, 10, 100);
    uint16_t far  = radar_rssi_to_radius(-80, 10, 100);
    ST_CHECK(near < mid && mid < far, "radar radius grows with distance");
    ST_CHECK(near >= 10 && far <= 100, "radar radius within clamp");
    ST_CHECK(radar_rssi_to_radius(-20, 10, 100) == near, "very strong clamps near");
    ST_CHECK(radar_rssi_to_radius(-99, 10, 100) == far,  "very weak clamps far");
    ST_CHECK(radar_hash_to_angle(0xABCD1234) == radar_hash_to_angle(0xABCD1234),
             "angle stable per hash");
    ST_CHECK(radar_hash_to_angle(0xFFFFFFFF) < 360, "angle in range");
    int x, y;
    radar_polar_to_xy(120, 160, 50, 0, &x, &y);
    ST_CHECK(x == 170 && y == 160, "0deg due east");
    radar_polar_to_xy(120, 160, 50, 90, &x, &y);
    ST_CHECK(x >= 119 && x <= 121 && y >= 109 && y <= 111, "90deg due north");
}

static void test_radar_ui(void)
{
    radar_ui_t ui;
    // Reset lands on the HOME dashboard grid (not radar), backlight on.
    radar_ui_reset(&ui, 1000, 0);
    ST_CHECK(ui.view == RADAR_VIEW_HOME && ui.backlight_on, "reset: home + bl on");
    // A HOME sigil tap jumps straight to the chosen view; a tap in a sub-view returns HOME.
    radar_ui_select_view(&ui, RADAR_VIEW_DETAIL, 1100);  ST_CHECK(ui.view == RADAR_VIEW_DETAIL,  "select -> detail");
    radar_ui_select_view(&ui, RADAR_VIEW_LIBRARY, 1200); ST_CHECK(ui.view == RADAR_VIEW_LIBRARY, "select -> library");
    radar_ui_on_input(&ui, 1300);                        ST_CHECK(ui.view == RADAR_VIEW_HOME,    "input -> back home");
    // Idle on a data view returns to HOME while the crowd is clear.
    radar_ui_select_view(&ui, RADAR_VIEW_STATS, 2000);
    radar_ui_on_tick(&ui, 2000 + RADAR_VIEW_IDLE_MS + 1, 0);
    ST_CHECK(ui.view == RADAR_VIEW_HOME, "idle returns to home");
    radar_ui_on_tick(&ui, 2000 + RADAR_BL_IDLE_MS + 2, 0);
    ST_CHECK(!ui.backlight_on, "clear + idle sleeps backlight");
    radar_ui_on_tick(&ui, 999999, 1);
    ST_CHECK(ui.backlight_on && ui.view == RADAR_VIEW_RADAR, "new follower wakes + radar");

    // A new follower must NOT yank the view to radar while the user is actively navigating
    // (recent input) -- the "random snap-back while reading a page" bug. It still wakes the
    // backlight, and once the user goes idle a new follower does jump to radar as an alert.
    radar_ui_reset(&ui, 10000, 0);
    radar_ui_select_view(&ui, RADAR_VIEW_DETAIL, 10100);   // actively navigating
    radar_ui_on_tick(&ui, 10200, 1);                       // new follower arrives mid-navigation
    ST_CHECK(ui.view == RADAR_VIEW_DETAIL, "new follower keeps page while navigating");
    ST_CHECK(ui.backlight_on, "new follower still wakes backlight while navigating");
    radar_ui_on_tick(&ui, 10200 + RADAR_VIEW_IDLE_MS + 1, 2);
    ST_CHECK(ui.view == RADAR_VIEW_RADAR, "new follower jumps to radar once idle");

    // CONTROL view is reached by a direct tap and has an independent preset selector.
    radar_ui_reset(&ui, 20000, 0);
    radar_ui_select_view(&ui, RADAR_VIEW_CONTROL, 20000);
    ST_CHECK(ui.view == RADAR_VIEW_CONTROL, "select reaches CONTROL");
    uint8_t p0 = ui.sel_preset;
    radar_ctrl_select_next(&ui);
    ST_CHECK(ui.sel_preset == (p0 + 1) % RADAR_CTRL_PRESET_COUNT, "select_next advances preset");
    radar_ctrl_mark_sent(&ui, 21000);
    ST_CHECK(ui.send_flash_ms == 21000, "mark_sent stamps the flash");
}

static void test_radar_wire(void)
{
    radar_wire_status_t st; memset(&st, 0, sizeof st);
    st.uptime_s = 4242; st.active_devices = 7; st.threat_count = 1;
    st.threats[0].hash = 0xDEADBEEF; st.threats[0].best_rssi = -44; st.threats[0].epochs = 5;

    uint8_t salt[4] = { 0xAA,0xBB,0xCC,0xDD };
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen = 0;
    int rc = radar_wire_seal(frame, &flen, RADAR_TYPE_STATUS,
                             (uint8_t*)&st, sizeof st, SIMULACRA_ESPNOW_KEY, salt, 100);
    ST_CHECK(rc == 0 && flen == RADAR_HDR_LEN + RADAR_NONCE_LEN + sizeof(st) + RADAR_TAG_LEN,
             "seal produces a full frame");
    ST_CHECK(frame[0] == RADAR_MAGIC0 && frame[3] == RADAR_TYPE_STATUS, "header intact");

    uint8_t type, pl[RADAR_FRAME_MAX], osalt[4]; size_t plen = 0; uint64_t ctr = 0;
    rc = radar_wire_open(frame, flen, SIMULACRA_ESPNOW_KEY, &type, pl, &plen, osalt, &ctr);
    ST_CHECK(rc == 0 && type == RADAR_TYPE_STATUS && plen == sizeof(st) && ctr == 100,
             "open round-trips");
    ST_CHECK(memcmp(pl, &st, sizeof st) == 0, "payload survives round-trip");

    frame[flen - 1] ^= 0x01;                                   // tamper the tag
    ST_CHECK(radar_wire_open(frame, flen, SIMULACRA_ESPNOW_KEY, &type, pl, &plen, osalt, &ctr) < 0,
             "tampered frame rejected");

    radar_replay_t rp = {0};
    ST_CHECK(radar_replay_ok(&rp, salt, 100), "first counter accepted");
    ST_CHECK(!radar_replay_ok(&rp, salt, 100), "replay of same counter rejected");
    ST_CHECK(!radar_replay_ok(&rp, salt, 99),  "older counter rejected");
    ST_CHECK(radar_replay_ok(&rp, salt, 101),  "newer counter accepted");
    uint8_t salt2[4] = { 1,2,3,4 };
    ST_CHECK(radar_replay_ok(&rp, salt2, 1),   "reboot (new salt) resets + accepts");
}

static void test_espnow_convert(void)
{
    webui_status_t w; memset(&w, 0, sizeof w);
    w.uptime_s = 61; w.decoy_paused = true; w.tx_degraded = true; w.active_devices = 6; w.roster_size = 64;
    w.probes_sent = 2048; w.epoch = 3; w.pop_ewma = 9; w.total_obs = 500;
    w.active_target = 6; w.threat_count = 1;
    w.threats[0].hash = 0xC0FFEE; w.threats[0].vendor = 0x004C;
    w.threats[0].best_rssi = -50; w.threats[0].epochs = 4;
    w.threats[0].kind = DETECT_KIND_KNOWN; w.threats[0].class_id = SIG_CLASS_TILE;
    w.threats[0].category = SIG_CAT_TRACKER; w.threats[0].confidence = 75;
    w.threats[0].sessions_seen = 3; w.threats[0].places_seen = 2;

    radar_wire_status_t r; espnow_status_from_webui(&r, &w);
    ST_CHECK(r.uptime_s == 61 && r.active_devices == 6 && r.probes_sent == 2048,
             "scalars copied");
    ST_CHECK((r.flags & 0x1) != 0, "paused flag packed");
    ST_CHECK((r.flags & 0x4) != 0, "tx-degraded health flag packed");
    ST_CHECK(r.threat_count == 1 && r.threats[0].hash == 0xC0FFEE &&
             r.threats[0].best_rssi == -50, "threat copied hash-only");
    ST_CHECK(r.threats[0].kind == DETECT_KIND_KNOWN && r.threats[0].class_id == SIG_CLASS_TILE,
             "convert: known fields carried");
    ST_CHECK(r.threats[0].confidence == 75, "convert: confidence carried");
    ST_CHECK(r.threats[0].sessions_seen == 3 && r.threats[0].places_seen == 2, "convert: recurrence carried");
}

static void test_sig_match(void)
{
    threat_sig_t s = {0};
    s.sig_id = 1; s.category = SIG_CAT_TRACKER; s.class_id = SIG_CLASS_AIRTAG;
    s.company_id = 0x004C; s.svc_uuid16 = 0x0000;
    s.addr_type_mask = SIG_ADDR_RPA | SIG_ADDR_NRPA;
    s.match_src = SIG_SRC_MFG_DATA; s.pat_off = 2; s.pat_len = 1;
    s.pattern[0] = 0x12; s.mask[0] = 0xFF; s.confidence = 80;

    uint8_t mfg[] = { 0x4C,0x00, 0x12, 0x19, 0xAA,0xBB,0xCC };
    sig_adv_fields_t adv = { .company_id = 0x004C, .svc_uuid16 = 0,
        .addr_type = SIG_ADDR_RPA, .mfg_data = mfg, .mfg_len = sizeof mfg,
        .svc_data = NULL, .svc_len = 0 };

    sig_hit_t hit;
    ST_CHECK(sig_match(&adv, &s, 1, &hit), "sig_match: airtag advert hits");
    ST_CHECK(hit.class_id == SIG_CLASS_AIRTAG, "sig_match: reports class");

    mfg[4] = 0x11; mfg[5] = 0x22;
    ST_CHECK(sig_match(&adv, &s, 1, &hit), "sig_match: robust to rotating bytes");

    uint8_t mfg2[] = { 0x4C,0x00, 0x07, 0x19, 0x00 };
    adv.mfg_data = mfg2; adv.mfg_len = sizeof mfg2;
    ST_CHECK(!sig_match(&adv, &s, 1, &hit), "sig_match: near-miss rejected");

    adv.mfg_data = mfg; adv.mfg_len = sizeof mfg; adv.company_id = 0x0075;
    ST_CHECK(!sig_match(&adv, &s, 1, &hit), "sig_match: company gate");

    adv.company_id = 0x004C; adv.mfg_len = 2;
    ST_CHECK(!sig_match(&adv, &s, 1, &hit), "sig_match: short advert bounds-safe");

    adv.mfg_len = sizeof mfg; adv.addr_type = SIG_ADDR_PUBLIC;
    ST_CHECK(!sig_match(&adv, &s, 1, &hit), "sig_match: addr-type gate");

    uint8_t rpa[6]  = {0,0,0,0,0, 0x40};
    uint8_t stat[6] = {0,0,0,0,0, 0xC0};
    ST_CHECK(sig_addr_type_from(1, rpa)  == SIG_ADDR_RPA,    "addr_type: RPA");
    ST_CHECK(sig_addr_type_from(1, stat) == SIG_ADDR_STATIC, "addr_type: static");
    ST_CHECK(sig_addr_type_from(0, rpa)  == SIG_ADDR_PUBLIC, "addr_type: public");
}

static void test_sig_regate(void)
{
    threat_sig_t s = {0};
    s.category = SIG_CAT_TRACKER; s.class_id = SIG_CLASS_TILE; s.match_src = SIG_SRC_SVC_DATA;
    s.pat_off = 0; s.pat_len = 2; s.confidence = 75;
    ST_CHECK(sig_regate(&s), "regate: well-formed accepted");

    threat_sig_t b1 = s; b1.pat_len = SIG_PAT_MAX + 1;
    ST_CHECK(!sig_regate(&b1), "regate: over-long pattern rejected");
    threat_sig_t b2 = s; b2.pat_off = SIG_PAT_MAX - 1; b2.pat_len = 4;
    ST_CHECK(!sig_regate(&b2), "regate: offset+len overrun rejected");
    threat_sig_t b3 = s; b3.category = SIG_CAT_COUNT;
    ST_CHECK(!sig_regate(&b3), "regate: bad category rejected");
    threat_sig_t b4 = s; b4.class_id = SIG_CLASS_COUNT;
    ST_CHECK(!sig_regate(&b4), "regate: bad class rejected");
    threat_sig_t b5 = s; b5.match_src = SIG_SRC_COUNT;
    ST_CHECK(!sig_regate(&b5), "regate: bad match_src rejected");
    threat_sig_t b6 = s; b6.confidence = 101;
    ST_CHECK(!sig_regate(&b6), "regate: confidence>100 rejected");
}

static void test_sig_db_blob(void)
{
    uint8_t psk[32]; for (int i=0;i<32;i++) psk[i]=(uint8_t)(i*7+1);
    uint8_t key[32]; sig_db_derive_key(psk, key);

    threat_sig_t recs[2] = {0};
    recs[0].sig_id = 1; recs[0].class_id = SIG_CLASS_AIRTAG; recs[0].company_id = 0x004C;
    recs[1].sig_id = 2; recs[1].class_id = SIG_CLASS_TILE;   recs[1].svc_uuid16 = 0xFEED;

    static uint8_t blob[sizeof(sig_db_hdr_t) + 2*sizeof(threat_sig_t)]; size_t blen;
    ST_CHECK(sig_db_seal(blob, &blen, recs, 2, 7, key) == 0, "sigdb: seal ok");

    threat_sig_t out[2]; uint16_t cnt = 0, ver = 0;
    ST_CHECK(sig_db_open(blob, blen, out, &cnt, &ver, key) == 0, "sigdb: open ok");
    ST_CHECK(cnt == 2 && ver == 7, "sigdb: count + content_version recovered");
    ST_CHECK(out[1].svc_uuid16 == 0xFEED, "sigdb: record bytes intact");

    blob[blen/2] ^= 0xFF;
    ST_CHECK(sig_db_open(blob, blen, out, &cnt, &ver, key) != 0, "sigdb: tamper rejected");

    uint8_t key2[32]; psk[0]^=1; sig_db_derive_key(psk, key2);
    ST_CHECK(sig_db_open(blob, blen, out, &cnt, &ver, key2) != 0, "sigdb: wrong key rejected");
}

static void test_sig_wire(void)
{
    threat_sig_t recs[3] = {0};
    recs[0].sig_id = 10; recs[1].sig_id = 11; recs[2].sig_id = 12;
    recs[2].company_id = 0x0075;

    uint8_t pl[RADAR_FRAME_MAX]; size_t plen;
    ST_CHECK(sig_wire_pack(pl, &plen, recs, 3, 42, 0, 2) == 0, "sigwire: pack ok");
    ST_CHECK(plen <= 218, "sigwire: chunk fits");

    threat_sig_t out[SIG_WIRE_RECS_PER_CHUNK]; uint8_t nr; sig_chunk_hdr_t h;
    ST_CHECK(sig_wire_unpack(pl, plen, out, &nr, &h) == 0, "sigwire: unpack ok");
    ST_CHECK(nr == 3 && h.content_version == 42 && h.chunk_count == 2, "sigwire: hdr recovered");
    ST_CHECK(out[2].company_id == 0x0075, "sigwire: record bytes intact");

    ST_CHECK(sig_wire_pack(pl, &plen, recs, SIG_WIRE_RECS_PER_CHUNK + 1, 1, 0, 1) < 0,
             "sigwire: over-capacity pack rejected");
    ST_CHECK(sig_wire_unpack(pl, 2, out, &nr, &h) < 0, "sigwire: short payload rejected");
}

static void test_sig_seed(void)
{
    threat_sig_t db[SIG_DB_CAP];
    size_t n = sig_seed_copy(db, SIG_DB_CAP);
    ST_CHECK(n >= 3, "seed: at least airtag/smarttag/tile");
    ST_CHECK(sig_seed_version() >= 1, "seed: content_version set");
    for (size_t i = 0; i < n; i++) ST_CHECK(sig_regate(&db[i]), "seed: every record re-gates clean");

    uint8_t mfg[] = { 0x4C,0x00, 0x12, 0x19, 0x10,0x00,0x00 };
    sig_adv_fields_t adv = { .company_id = 0x004C, .svc_uuid16 = 0, .addr_type = SIG_ADDR_RPA,
        .mfg_data = mfg, .mfg_len = sizeof mfg, .svc_data = NULL, .svc_len = 0 };
    sig_hit_t hit;
    ST_CHECK(sig_match(&adv, db, n, &hit) && hit.class_id == SIG_CLASS_AIRTAG, "seed: airtag hit");

    uint8_t svc[] = { 0xED,0xFE, 0x02,0x00,0x0C };
    sig_adv_fields_t tadv = { .company_id = 0xFFFF, .svc_uuid16 = 0xFEED, .addr_type = SIG_ADDR_PUBLIC,
        .mfg_data = NULL, .mfg_len = 0, .svc_data = svc, .svc_len = sizeof svc };
    ST_CHECK(sig_match(&tadv, db, n, &hit) && hit.class_id == SIG_CLASS_TILE, "seed: tile hit");

    ST_CHECK(sig_class_name(SIG_CLASS_AIRTAG)[0] != '\0', "class name: airtag non-empty");
}

static int detect_threat_find_kind(uint32_t hash) {
    for (size_t i = 0; i < detect_threat_count(); i++) {
        detect_threat_t t; if (detect_threat_at(i, &t) && t.hash == hash) return t.kind;
    }
    return -1;
}

static void test_detect_known(void)
{
    detect_reset();
    ST_CHECK(detect_note_known(0xAAAA1111, -55, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 3) == DETECT_CONFIRM,
             "known: first hit confirms");
    ST_CHECK(detect_threat_count() == 1, "known: one threat row");
    detect_threat_t t;
    ST_CHECK(detect_threat_at(0, &t) && t.kind == DETECT_KIND_KNOWN, "known: row is KIND_KNOWN");
    ST_CHECK(t.class_id == SIG_CLASS_AIRTAG && t.confidence == 80, "known: class + confidence stored");

    ST_CHECK(detect_note_known(0xAAAA1111, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 4) == DETECT_KNOWN,
             "known: repeat updates");
    ST_CHECK(detect_threat_count() == 1, "known: no duplicate");
    detect_threat_at(0, &t); ST_CHECK(t.best_rssi == -40, "known: rssi improved");

    detect_reset();
    for (int i = 0; i < DETECT_MAX_THREATS; i++)
        detect_note_known(0xB0000000u + i, -60, SIG_CLASS_TILE, SIG_CAT_TRACKER, 75, 1);
    for (uint16_t e = 1; e <= 3; e++) { detect_observe(0xF0F0F0F0, -50, 0x1234, e); detect_observe(0xF0F0F0F0, -50, 0x1234, e); }
    ST_CHECK(detect_threat_find_kind(0xF0F0F0F0) == DETECT_KIND_FOLLOWER, "known: follower admitted over KNOWN");
    for (int i = 0; i < DETECT_MAX_THREATS; i++)
        detect_note_known(0xC0000000u + i, -60, SIG_CLASS_TILE, SIG_CAT_TRACKER, 75, 5);
    ST_CHECK(detect_threat_find_kind(0xF0F0F0F0) == DETECT_KIND_FOLLOWER, "known: follower not evicted by KNOWN");
}

static void test_sig_store(void)
{
    sig_store_load_seed();
    ST_CHECK(sig_store_count() >= 3, "store: seed loaded");
    ST_CHECK(sig_store_version() == sig_seed_version(), "store: seed version");

    threat_sig_t one[1] = {0};
    one[0].sig_id = 99; one[0].class_id = SIG_CLASS_TILE; one[0].match_src = SIG_SRC_SVC_DATA;
    one[0].svc_uuid16 = 0xFEED; one[0].pat_len = 0; one[0].confidence = 75;
    ST_CHECK(sig_store_adopt(one, 1, sig_seed_version() + 1), "store: newer version adopted");
    ST_CHECK(sig_store_count() == 1 && sig_store_version() == sig_seed_version()+1, "store: swapped wholesale");

    ST_CHECK(!sig_store_adopt(one, 1, sig_seed_version()), "store: older version ignored");

    threat_sig_t two[2] = {0};
    two[0] = one[0]; two[0].sig_id = 5;
    two[1] = one[0]; two[1].sig_id = 6; two[1].pat_len = SIG_PAT_MAX + 5;
    ST_CHECK(sig_store_adopt(two, 2, sig_seed_version() + 2), "store: adopt with one bad record");
    ST_CHECK(sig_store_count() == 1, "store: bad record re-gated out");
}

static void test_fleet(void)
{
    fleet_reset();
    uint8_t set[3][6] = { {1,2,3,4,5,6}, {7,7,7,7,7,7}, {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF} };
    fleet_note_peer_macs(set, 2, 1000);
    ST_CHECK(fleet_mac_excluded(set[0], 1000) && fleet_mac_excluded(set[1], 1000), "fleet: noted peers excluded");
    ST_CHECK(!fleet_mac_excluded(set[2], 1000), "fleet: unknown mac not excluded");
    ST_CHECK(fleet_peer_count(1000) == 2, "fleet: two peers tracked");

    // refresh just before expiry keeps a peer alive; the un-refreshed one expires
    fleet_note_peer_macs(set, 1, 1000 + FLEET_MAC_TTL_MS - 1);
    ST_CHECK(fleet_mac_excluded(set[0], 1000 + FLEET_MAC_TTL_MS - 1), "fleet: refresh keeps peer alive");
    ST_CHECK(!fleet_mac_excluded(set[1], 1000 + FLEET_MAC_TTL_MS + 1), "fleet: stale peer expires");

    // wire round-trip
    uint8_t buf[RADAR_FRAME_MAX]; size_t plen = fleet_macs_pack(buf, sizeof buf, set, 3);
    ST_CHECK(plen == 1 + 3*6, "fleet: pack length");
    uint8_t out[8][6]; size_t n = fleet_macs_unpack(buf, plen, out, 8);
    ST_CHECK(n == 3 && memcmp(out[2], set[2], 6) == 0, "fleet: unpack round-trips");
    ST_CHECK(fleet_macs_unpack(buf, 2, out, 8) == 0, "fleet: truncated payload rejected");
}

static void test_rf_model_decay(void)
{
    // Rolling window: a vendor seen heavily once then never again must fade below a vendor that
    // keeps being seen, so the model tracks the CURRENT room instead of all-time history.
    rf_model_t m; rf_model_reset(&m);
    for (int i = 0; i < 200; i++) rf_model_observe(&m, 0x00AA, -50, 3, 500);   // vendor A, once
    int ia = rf_vendor_index(&m, 0x00AA);
    uint32_t a1 = m.vendors[ia].count;
    for (int s = 0; s < 8; s++) {                                              // 8 sweeps of vendor B only
        for (int i = 0; i < 200; i++) rf_model_observe(&m, 0x00BB, -50, 3, 500);
        rf_model_decay(&m);
    }
    ia = rf_vendor_index(&m, 0x00AA);
    int ib = rf_vendor_index(&m, 0x00BB);
    uint32_t a2 = (ia >= 0) ? m.vendors[ia].count : 0;
    uint32_t b2 = (ib >= 0) ? m.vendors[ib].count : 0;
    ST_CHECK(a2 < a1, "decay: stale vendor faded from the model");
    ST_CHECK(b2 > a2, "decay: recently-present vendor outweighs the stale one");
}

static void test_generate_diversity_floor(void)
{
    // A model skewed to a single loud vendor (a room full of Galaxy Buds) must NOT produce a
    // single-manufacturer synthetic crowd — the generation diversity floor fills the overflow
    // from varied built-in templates.
    rf_model_t m; rf_model_reset(&m);
    for (int i = 0; i < 200; i++) rf_model_observe(&m, 0x0075, -50, 3, 500);   // Samsung only
    static identity_t roster[64];
    size_t built = generate_roster(&m, roster, 64);
    ST_CHECK(built >= 60, "diversity: most identities built");

    size_t samsung = 0; uint16_t seen[32]; size_t distinct = 0;
    for (size_t i = 0; i < 64; i++) {
        if (roster[i].company_id == 0x0075) samsung++;
        size_t j; for (j = 0; j < distinct; j++) if (seen[j] == roster[i].company_id) break;
        if (j == distinct && distinct < 32) seen[distinct++] = roster[i].company_id;
    }
    // Proportional redirect targets ~GEN_MAX_VENDOR_PCT; a 65% ceiling clears the sampling variance
    // while still cleanly failing the old monoculture (was 64/64 = 100%).
    ST_CHECK(samsung * 100 <= 64 * 65, "diversity: dominant vendor no longer a monoculture");
    ST_CHECK(distinct >= 4, "diversity: crowd spans multiple manufacturers");
}

static void test_escalation_ladder(void)
{
    ST_CHECK(threat_escalation_level(1,1) == ESCALATION_NEW,        "esc: (1,1) NEW");
    ST_CHECK(threat_escalation_level(1,2) == ESCALATION_NEW,        "esc: (1,2) NEW");
    ST_CHECK(threat_escalation_level(2,1) == ESCALATION_RECURRING,  "esc: (2,1) RECURRING (2nd session)");
    ST_CHECK(threat_escalation_level(1,3) == ESCALATION_RECURRING,  "esc: (1,3) RECURRING (breadth)");
    ST_CHECK(threat_escalation_level(3,1) == ESCALATION_RECURRING,  "esc: (3,1) not persistent (1 place)");
    ST_CHECK(threat_escalation_level(3,2) == ESCALATION_PERSISTENT, "esc: (3,2) PERSISTENT");
    ST_CHECK(threat_escalation_level(9,9) == ESCALATION_PERSISTENT, "esc: saturated PERSISTENT");
    ST_CHECK(escalation_name(ESCALATION_PERSISTENT)[0] == 'P',      "esc: name P");
}

static void test_escalation_recurrence(void)
{
    detect_reset();
    detect_set_session(1);
    for (uint16_t e = 1; e <= 3; e++) { detect_on_epoch_change(e);
        detect_observe(0xABCD, -50, 0x0075, e); detect_observe(0xABCD, -50, 0x0075, e); }
    detect_threat_t t;
    ST_CHECK(detect_threat_at(0, &t) && t.sessions_seen == 1 && t.places_seen == 1, "recur: fresh = 1/1");
    ST_CHECK(threat_escalation_level(t.sessions_seen, t.places_seen) == ESCALATION_NEW, "recur: fresh NEW");

    detect_observe(0xABCD, -50, 0x0075, 4);
    detect_observe(0xABCD, -50, 0x0075, 5);
    detect_threat_at(0, &t);
    ST_CHECK(t.sessions_seen == 1 && t.places_seen == 3, "recur: places grow within a session");
    ST_CHECK(threat_escalation_level(t.sessions_seen, t.places_seen) == ESCALATION_RECURRING, "recur: breadth RECURRING");

    detect_observe(0xABCD, -50, 0x0075, 5);
    detect_threat_at(0, &t);
    ST_CHECK(t.places_seen == 3, "recur: same context no double count");

    detect_set_session(2);
    detect_observe(0xABCD, -50, 0x0075, 6);
    detect_threat_at(0, &t);
    ST_CHECK(t.sessions_seen == 2, "recur: new session bumps sessions_seen");

    detect_reset(); detect_set_session(1);
    detect_note_known(0x1234, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 1);
    detect_note_known(0x1234, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 2);   // new place
    detect_set_session(2); detect_note_known(0x1234, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 3);
    detect_set_session(3); detect_note_known(0x1234, -40, SIG_CLASS_AIRTAG, SIG_CAT_TRACKER, 80, 4);
    detect_threat_at(0, &t);
    ST_CHECK(t.sessions_seen == 3 && t.places_seen >= 2, "recur: KNOWN escalates across sessions");
    ST_CHECK(threat_escalation_level(t.sessions_seen, t.places_seen) == ESCALATION_PERSISTENT, "recur: KNOWN PERSISTENT");
}

static void test_settings_resolve(void)
{
    sim_settings_t s;
    ST_CHECK(sim_settings_resolve(SIM_PRESET_NORMAL, 16, &s) == 0, "resolve NORMAL ok");
    ST_CHECK(s.active_target == 16 && !s.paused && s.accel == 1.0f, "NORMAL fills ceiling, running");

    ST_CHECK(sim_settings_resolve(SIM_PRESET_STEALTH, 16, &s) == 0, "resolve STEALTH ok");
    ST_CHECK(s.active_target == 6 && s.dwell_min_ms >= 300000, "STEALTH ~40% ceiling, long dwell");

    ST_CHECK(sim_settings_resolve(SIM_PRESET_MAX, 16, &s) == 0, "resolve MAX ok");
    ST_CHECK(s.active_target == 16 && s.accel > 2.0f && s.dwell_max_ms <= 120000, "MAX cranks turnover");

    ST_CHECK(sim_settings_resolve(SIM_PRESET_PAUSE, 16, &s) == 0, "resolve PAUSE ok");
    ST_CHECK(s.paused && s.active_target == 16, "PAUSE freezes rotation, crowd stays on-air");

    ST_CHECK(sim_settings_resolve(SIM_PRESET_COUNT, 16, &s) == -1, "bad preset rejected");

    // Clamp floors: a hostile 'target=0, dwell=0' can't cross safe bounds.
    sim_settings_t bad = { .active_target = 0, .paused = false, .accel = 9.0f,
                           .dwell_min_ms = 0, .dwell_max_ms = 5, .cooldown_min_ms = 0, .cooldown_max_ms = 0 };
    sim_settings_clamp(&bad, 16);
    ST_CHECK(bad.active_target >= SIM_TARGET_FLOOR, "clamp raises target to floor");
    ST_CHECK(bad.dwell_min_ms >= 30000 && bad.accel <= 4.0f && bad.cooldown_min_ms >= 300000, "clamp bounds dwell/accel/cooldown");

    // Ceiling honored on a smaller board (Shade-like ceiling).
    ST_CHECK(sim_settings_resolve(SIM_PRESET_MAX, 8, &s) == 0 && s.active_target == 8, "MAX clamps to board ceiling");
}

static void test_settings_apply(void)
{
    roster_init(); churn_set_apply(noop_apply);
    // Milestone A: churn target/dwell are inert no-ops now (lifetime owned by ble_devices),
    // so we only assert the settings behavior that still drives churn: pause/resume.
    sim_settings_apply_preset(SIM_PRESET_STEALTH);
    ST_CHECK(!churn_paused(), "STEALTH is running");

    sim_settings_apply_preset(SIM_PRESET_PAUSE);
    ST_CHECK(churn_paused(), "apply PAUSE pauses churn");

    ST_CHECK(sim_settings_apply_preset(SIM_PRESET_COUNT) == -1, "apply bad preset rejected");

    sim_settings_t g; sim_settings_get(&g);
    ST_CHECK(g.paused, "get reflects last applied (PAUSE)");

    // Restore NORMAL so subsequent tests run with defaults.
    sim_settings_apply_preset(SIM_PRESET_NORMAL);
}

static void test_config_wire(void)
{
    uint8_t pk[32], sk[64];
    crypto_sign_keypair(pk, sk);                       // ephemeral test keypair
    uint8_t nonce[12]; for (int i=0;i<12;i++) nonce[i] = (uint8_t)(i*7+1);
    config_cmd_t cmd = { .version = CONFIG_WIRE_VER, .preset_id = 3 };

    uint8_t pl[CONFIG_WIRE_PAYLOAD_LEN];
    int n = config_wire_pack_signed(pl, sizeof pl, &cmd, nonce, sk);
    ST_CHECK(n == CONFIG_WIRE_PAYLOAD_LEN, "pack returns payload len");

    config_cmd_t got;
    ST_CHECK(config_wire_open_signed(pl, n, nonce, pk, &got) == 0, "open verifies good sig");
    ST_CHECK(got.preset_id == 3 && got.version == CONFIG_WIRE_VER, "open recovers cmd");

    pl[0] ^= 0x01;                                      // tamper cmd byte
    ST_CHECK(config_wire_open_signed(pl, n, nonce, pk, &got) != 0, "tampered cmd fails verify");
    pl[0] ^= 0x01;                                      // restore

    uint8_t nonce2[12]; memcpy(nonce2, nonce, 12); nonce2[0] ^= 0x01;
    ST_CHECK(config_wire_open_signed(pl, n, nonce2, pk, &got) != 0, "nonce mismatch fails verify");
}

static void test_enroll_wire(void)
{
    uint8_t ctrl_pk[32], ctrl_sk[64];  crypto_sign_keypair(ctrl_pk, ctrl_sk);      // Vigil signing key
    uint8_t veph_pk[32], veph_sk[32];  crypto_box_keypair(veph_pk, veph_sk);        // Vigil ephemeral
    uint8_t id_pk[32],  id_sk[32];     crypto_box_keypair(id_pk, id_sk);            // decoy identity
    uint8_t nonce_v[24], nonce_d[24];
    for (int i = 0; i < 24; i++) { nonce_v[i] = (uint8_t)(i + 1); nonce_d[i] = (uint8_t)(0x40 + i); }
    uint32_t epoch = 7;
    uint8_t k_fleet[32]; for (int i = 0; i < 32; i++) k_fleet[i] = (uint8_t)(0x80 + i);

    // OFFER
    uint8_t offer[ENROLL_OFFER_LEN];
    ST_CHECK(enroll_offer_sign(offer, sizeof offer, veph_pk, nonce_v, epoch, ctrl_sk) == ENROLL_OFFER_LEN,
             "offer signs to expected len");
    uint8_t o_eph[32], o_nv[24]; uint32_t o_ep;
    ST_CHECK(enroll_offer_open(offer, ENROLL_OFFER_LEN, ctrl_pk, o_eph, o_nv, &o_ep) == 0, "offer verifies");
    ST_CHECK(memcmp(o_eph, veph_pk, 32) == 0 && memcmp(o_nv, nonce_v, 24) == 0 && o_ep == epoch,
             "offer fields recovered");
    offer[2] ^= 0x01;
    ST_CHECK(enroll_offer_open(offer, ENROLL_OFFER_LEN, ctrl_pk, o_eph, o_nv, &o_ep) != 0, "tampered offer rejected");
    offer[2] ^= 0x01;
    uint8_t wrong_pk[32], wrong_sk[64]; crypto_sign_keypair(wrong_pk, wrong_sk);
    ST_CHECK(enroll_offer_open(offer, ENROLL_OFFER_LEN, wrong_pk, o_eph, o_nv, &o_ep) != 0, "wrong ctrl_pk rejected");

    // REQUEST
    uint8_t req[ENROLL_REQUEST_LEN];
    ST_CHECK(enroll_request_build(req, sizeof req, id_pk, id_sk, nonce_d, veph_pk, nonce_v) == ENROLL_REQUEST_LEN,
             "request builds to expected len");
    uint8_t r_idpk[32], r_nd[24], r_nv[24];
    ST_CHECK(enroll_request_open(req, ENROLL_REQUEST_LEN, veph_sk, r_idpk, r_nd, r_nv) == 0, "request opens");
    ST_CHECK(memcmp(r_idpk, id_pk, 32) == 0 && memcmp(r_nd, nonce_d, 24) == 0 && memcmp(r_nv, nonce_v, 24) == 0,
             "request fields recovered (id_pk, nonce_d, echoed nonce_v)");
    req[81] ^= 0x01;   // flip a ciphertext byte (first box byte)
    ST_CHECK(enroll_request_open(req, ENROLL_REQUEST_LEN, veph_sk, r_idpk, r_nd, r_nv) != 0, "tampered request rejected");
    req[81] ^= 0x01;

    // GRANT
    uint8_t grant[ENROLL_GRANT_LEN];
    ST_CHECK(enroll_grant_seal(grant, sizeof grant, id_pk, veph_sk, nonce_d, k_fleet, epoch) == ENROLL_GRANT_LEN,
             "grant seals to expected len");
    uint8_t g_key[32], g_nd[24]; uint32_t g_ep;
    ST_CHECK(enroll_grant_open(grant, ENROLL_GRANT_LEN, veph_pk, id_sk, g_key, &g_ep, g_nd) == 0, "grant opens");
    ST_CHECK(memcmp(g_key, k_fleet, 32) == 0 && g_ep == epoch && memcmp(g_nd, nonce_d, 24) == 0,
             "grant delivers key+epoch+nonce_d");
    uint8_t bad_pk[32], bad_sk[32]; crypto_box_keypair(bad_pk, bad_sk);
    ST_CHECK(enroll_grant_open(grant, ENROLL_GRANT_LEN, veph_pk, bad_sk, g_key, &g_ep, g_nd) != 0,
             "wrong id_sk cannot open grant");

    // --- identity authentication: the REQUEST must prove possession of id_sk, not just knowledge
    // of a public id_pk. Forge a request that CLAIMS a victim's id_pk in the clear but seals the
    // inner box with a different (attacker) secret; opening against the claimed id_pk must fail. ---
    uint8_t vic_pk[32], vic_sk[32]; crypto_box_keypair(vic_pk, vic_sk);
    uint8_t forged[ENROLL_REQUEST_LEN];
    ST_CHECK(enroll_request_build(forged, sizeof forged, vic_pk, id_sk, nonce_d, veph_pk, nonce_v)
             == ENROLL_REQUEST_LEN, "forged request builds (claims vic_pk, sealed by wrong id_sk)");
    ST_CHECK(enroll_request_open(forged, ENROLL_REQUEST_LEN, veph_sk, r_idpk, r_nd, r_nv) != 0,
             "request claiming a foreign id_pk (sealed by wrong id_sk) is rejected");

    // --- per-window binding: a REQUEST is boxed to the current window's Vigil ephemeral, so a
    // DIFFERENT window's ephemeral secret cannot open it. This is what makes a captured REQUEST
    // from a previous window useless against a new window (cross-window replay protection). ---
    uint8_t veph2_pk[32], veph2_sk[32]; crypto_box_keypair(veph2_pk, veph2_sk);
    ST_CHECK(enroll_request_open(req, ENROLL_REQUEST_LEN, veph2_sk, r_idpk, r_nd, r_nv) != 0,
             "request bound to one ephemeral is rejected by a different ephemeral");

    // --- granter authentication: the GRANT is sealed FROM the Vigil ephemeral, so opening it with
    // the wrong ephemeral pubkey as the sender must fail (a decoy won't accept a key from an
    // impostor that isn't the Vigil ephemeral it just requested from). ---
    ST_CHECK(enroll_grant_open(grant, ENROLL_GRANT_LEN, veph2_pk, id_sk, g_key, &g_ep, g_nd) != 0,
             "grant from a different ephemeral (wrong granter) is rejected");
}

#ifdef SIMULACRA_FLEET_PROVISION
static void test_fleet_key(void)
{
    fleet_key_init();
    ST_CHECK(fleet_id_pk() != NULL, "identity pubkey available after init");
    char fp1[24]; fleet_id_fingerprint(fp1, sizeof fp1);
    ST_CHECK(strlen(fp1) == 19, "fingerprint formatted xxxx-xxxx-xxxx-xxxx");

    uint8_t k[32]; for (int i = 0; i < 32; i++) k[i] = (uint8_t)(i * 3 + 1);
    fleet_key_set(k, 5);
    ST_CHECK(fleet_key_have() && fleet_key_get() && memcmp(fleet_key_get(), k, 32) == 0,
             "fleet key stored + readable");
    ST_CHECK(fleet_key_epoch() == 5, "epoch recorded");

    uint8_t k2[32]; for (int i = 0; i < 32; i++) k2[i] = (uint8_t)(i * 7 + 2);
    fleet_key_set(k2, 6);
    ST_CHECK(memcmp(fleet_key_get(), k2, 32) == 0, "rotated key becomes current");
    ST_CHECK(fleet_key_prev() && memcmp(fleet_key_prev(), k, 32) == 0,
             "old key retained as prev during grace");
}

static void test_fleet_open_grace(void)
{
    uint8_t kold[32], knew[32];
    for (int i = 0; i < 32; i++) { kold[i] = (uint8_t)(i + 10); knew[i] = (uint8_t)(i + 200); }
    fleet_key_set(kold, 10);
    fleet_key_set(knew, 11);            // rotate: prev = kold, current = knew

    uint8_t salt[4] = { 1, 2, 3, 4 }; uint8_t payload[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    ST_CHECK(radar_wire_seal(frame, &flen, RADAR_TYPE_STATUS, payload, sizeof payload, kold, salt, 1) == 0,
             "grace: seal a frame under the old key");
    uint8_t type, pl[RADAR_FRAME_MAX], os[4]; size_t plen; uint64_t ctr;
    ST_CHECK(espnow_open_any(frame, flen, &type, pl, &plen, os, &ctr) == 0,
             "grace: open_any falls back to previous key");
    ST_CHECK(type == RADAR_TYPE_STATUS && plen == sizeof payload && memcmp(pl, payload, plen) == 0,
             "grace: payload recovered via prev key");

    uint8_t rnd[32]; for (int i = 0; i < 32; i++) rnd[i] = (uint8_t)(i * 5 + 3);
    ST_CHECK(radar_wire_seal(frame, &flen, RADAR_TYPE_STATUS, payload, sizeof payload, rnd, salt, 2) == 0,
             "grace: seal under an unrelated key");
    ST_CHECK(espnow_open_any(frame, flen, &type, pl, &plen, os, &ctr) != 0,
             "grace: unknown key rejected by open_any");
}
#endif

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
        if (!is_valid_random_top2(id->addr[5])) macs_ok = false;   // static/RPA/NRPA subtype
        if (id->payload_len == 0) payload_ok = false;
    }
    ST_CHECK(macs_ok, "roster: every MAC is a valid random subtype (static/RPA/NRPA)");
    ST_CHECK(payload_ok, "roster: every identity has a non-empty payload");

    identity_t *c = roster_promote_candidate(0);
    ST_CHECK(c != NULL, "promote_candidate returns an identity");

    // --- churn lifecycle ---
    test_churn_present();
    test_timeslice();
    test_settings_resolve();
    test_settings_apply();
    test_config_wire();
    test_enroll_wire();
#ifdef SIMULACRA_FLEET_PROVISION
    test_fleet_key();
    test_fleet_open_grace();
#endif

    // --- M4 templates ---
    test_templates();
    test_law3();
    test_learn_strip();
    test_learn_render();
    test_learn_store();
    test_learn_pipeline();
    test_learn_nvs();
    test_learn_generate();
    test_learn_wire();
    test_learn_regate();
    test_learn_merge_wire();
    test_learn_snapshot_ingest();
    test_learn_db_key();
    test_learn_db_blob();
    test_learn_top_n();
    test_sig_match();
    test_sig_regate();
    test_sig_db_blob();
    test_sig_wire();
    test_sig_seed();
    test_sig_store();
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
    test_detect_known();
    test_escalation_ladder();
    test_escalation_recurrence();
    test_rf_model_decay();
    test_fleet();
    test_generate_diversity_floor();
    test_webui_json();
    test_radar_geometry();
    test_radar_ui();
    test_radar_wire();
    test_espnow_convert();

    ESP_LOGW(TAG, "SELFTEST: %s (%d/%d)", s_fail ? "FAIL" : "PASS",
             s_total - s_fail, s_total);
    return s_fail;
}
