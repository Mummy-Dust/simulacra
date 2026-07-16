#include <string.h>
#include "observe.h"
#include "learn.h"
#include "sig_match.h"
#include "sig_store.h"
#include "fleet.h"
#include "esp_log.h"

// Self-learning template harvester: default ON, gated so it can be built out.
#ifndef SIMULACRA_LEARN
#define SIMULACRA_LEARN 1
#endif
#ifndef OBSERVE_LOG_RSSI
#define OBSERVE_LOG_RSSI 0   // 1 = log per-advert RSSI for the decoy_audit physical slice (bench only)
#endif
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

#define OBS_TABLE_CAP 256

typedef struct { uint32_t hash; uint32_t first_ms; uint32_t last_ms; bool used; } obs_entry_t;

static obs_entry_t s_tbl[OBS_TABLE_CAP];
static uint32_t    s_salt;
static uint32_t    s_arrivals;     // new distinct hashes this window
static bool        s_saturated;

void observe_reset_ephemeral(uint32_t boot_salt)
{
    memset(s_tbl, 0, sizeof(s_tbl));
    s_salt = boot_salt;
    s_arrivals = 0;
    s_saturated = false;
}

uint32_t observe_hash_mac(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_salt;     // FNV-1a offset basis, salted
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

void observe_ingest(rf_model_t *m, const uint8_t mac[6], uint32_t now_ms,
                    uint16_t company_id, int8_t rssi, uint8_t pdu_type)
{
    uint32_t h = observe_hash_mac(mac);    // MAC consumed here, never stored
    int32_t interval = -1;
    obs_entry_t *slot = NULL, *freep = NULL;
    for (size_t i = 0; i < OBS_TABLE_CAP; i++) {
        if (s_tbl[i].used && s_tbl[i].hash == h) { slot = &s_tbl[i]; break; }
        if (!s_tbl[i].used && !freep) freep = &s_tbl[i];
    }
    if (slot) {
        interval = (int32_t)(now_ms - slot->last_ms);
        slot->last_ms = now_ms;
    } else if (freep) {
        freep->used = true; freep->hash = h; freep->first_ms = now_ms; freep->last_ms = now_ms;
        s_arrivals++;
    } else {
        s_saturated = true;                // full: still counted in the model, just not deduped
    }
    rf_model_observe(m, company_id, rssi, pdu_type, interval);
}

void observe_end_sweep(rf_model_t *m, uint32_t window_ms)
{
    uint32_t distinct = 0;
    for (size_t i = 0; i < OBS_TABLE_CAP; i++) if (s_tbl[i].used) distinct++;
    rf_model_end_sweep(m, distinct, window_ms, s_arrivals);
    rf_model_decay(m);                     // rolling window: fade old obs so the model tracks NOW
    memset(s_tbl, 0, sizeof(s_tbl));       // wipe ephemeral identifiers
    s_arrivals = 0;
    s_saturated = false;
#if SIMULACRA_LEARN
    // Advance the learn sweep (age-out + wipe transient candidates) and persist
    // the learned library periodically (debounced to spare NVS wear).
    static uint16_t s_learn_sweep, s_learn_persist;
    learn_end_sweep(++s_learn_sweep);
    if (++s_learn_persist >= 8) { s_learn_persist = 0; learn_save_nvs(); }
#endif
}

size_t observe_ephemeral_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < OBS_TABLE_CAP; i++) if (s_tbl[i].used) n++;
    return n;
}

// --- live radio path ---

static const char *TAG = "observe";
#define OBS_SWEEP_MS       15000   // observation window (15 s: short enough for the ~13 s reader)
#define OBS_PERSIST_EVERY  1       // persist + dump every N sweeps
#define OBS_SCAN_ITVL      0x00A0  // 100 ms in 0.625 ms units
#define OBS_SCAN_WIN       0x00A0  // 100 ms window == interval -> continuous passive scan

static rf_model_t s_model;
static uint32_t   s_sweep_start_ms;
static uint32_t   s_persist_ctr;
static int        s_scan_rc = -1;          // last ble_gap_disc() result (liveness/diag)
static bool       s_window_mode;           // true while observe_window() owns the scan
static observe_report_cb_t s_report_cb;    // M9: raw-report tap (fired before the MAC is hashed)

static void observe_maybe_close_sweep(uint32_t now)
{
    if (now - s_sweep_start_ms < OBS_SWEEP_MS) return;
    observe_end_sweep(&s_model, now - s_sweep_start_ms);
    s_sweep_start_ms = now;
    ESP_LOGW(TAG, "[sweep %u] pop=%u arr/min=%u obs=%u",
             (unsigned)s_model.sweeps, (unsigned)(s_model.pop_ewma + 0.5f),
             (unsigned)(s_model.arrival_per_min + 0.5f), (unsigned)s_model.total_obs);
    if (++s_persist_ctr >= OBS_PERSIST_EVERY) {
        s_persist_ctr = 0;
        rf_model_save_nvs(&s_model);
        rf_model_dump(&s_model);
    }
}

static int observe_gap_event(struct ble_gap_event *event, void *arg);

// Start extended passive discovery. With CONFIG_BT_NIMBLE_EXT_ADV enabled (the decoy needs it),
// scan reports arrive as BLE_GAP_EVENT_EXT_DISC -- legacy ble_gap_disc reports are never delivered,
// so the scan must be started via ble_gap_ext_disc().
static void observe_start_scan(void)
{
    struct ble_gap_ext_disc_params uncoded;
    memset(&uncoded, 0, sizeof(uncoded));
    uncoded.passive = 1;                       // never send scan requests -> never reveal ourselves
    uncoded.itvl    = OBS_SCAN_ITVL;
    uncoded.window  = OBS_SCAN_WIN;
    // duration 0 = forever, period 0 = none, filter_duplicates 0 = report every packet (we dedup)
    s_scan_rc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, 0, 0, 0, 0, 0, &uncoded, NULL,
                                 observe_gap_event, NULL);
}

static int observe_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {        // restart only in standalone mode
        if (!s_window_mode) observe_start_scan();
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_EXT_DISC) return 0;

    struct ble_gap_ext_disc_desc *d = &event->ext_disc;
    uint16_t company = RF_VENDOR_UNKNOWN;
    struct ble_hs_adv_fields f;
    bool parsed = (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0);
    bool has_mfg = parsed && f.mfg_data && f.mfg_data_len >= 2;
    if (has_mfg) company = (uint16_t)(f.mfg_data[0] | (f.mfg_data[1] << 8));

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // Fleet self-exclusion: a fleet-mate's synthetic advert -> skip detect/learn/model entirely
    // (the sweep still closes so windows time out correctly).
    if (fleet_mac_excluded(d->addr.val, now)) {
        if (!s_window_mode) observe_maybe_close_sweep(now);
        return 0;
    }

    // M10 fingerprint match against the RAM signature store (empty unless the decoy seeded it).
    const sig_hit_t *hitp = NULL;
    sig_hit_t hit;
    if (parsed && sig_store_count() > 0) {
        uint16_t svc16 = (f.num_uuids16 > 0) ? f.uuids16[0].value : 0x0000;
        const uint8_t *svcd = NULL; uint8_t svcd_len = 0;
        if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 2) {
            svcd = f.svc_data_uuid16; svcd_len = f.svc_data_uuid16_len;
            if (svc16 == 0) svc16 = (uint16_t)(svcd[0] | (svcd[1] << 8));
        }
        sig_adv_fields_t sf = {
            .company_id = has_mfg ? company : 0xFFFF, .svc_uuid16 = svc16,
            .addr_type = sig_addr_type_from(d->addr.type, d->addr.val),
            .mfg_data = f.mfg_data, .mfg_len = f.mfg_data_len,
            .svc_data = svcd, .svc_len = svcd_len,
        };
        if (sig_match(&sf, sig_store_db(), sig_store_count(), &hit)) hitp = &hit;
    }

    // legacy_event_type is the PDU type for legacy ads; non-legacy ext ads clamp to the last bin.
    if (s_report_cb) s_report_cb(d->addr.val, d->rssi, company, hitp);   // M9 tap: raw MAC still live here
#if OBSERVE_LOG_RSSI
    ESP_LOGW(TAG, "obs rssi=%d company=0x%04x", (int)d->rssi, (unsigned)company);   // decoy_audit physical slice
#endif
#if SIMULACRA_LEARN
    learn_offer(observe_hash_mac(d->addr.val), d->data, d->length_data, company, now);
#endif
    observe_ingest(&s_model, d->addr.val, now, company, d->rssi, d->legacy_event_type);  // MAC dropped inside
    if (!s_window_mode) observe_maybe_close_sweep(now);      // window mode closes explicitly
    return 0;
}

void observe_set_report_cb(observe_report_cb_t cb) { s_report_cb = cb; }

void observe_start(uint32_t boot_salt)
{
    observe_reset_ephemeral(boot_salt);
    if (rf_model_load_nvs(&s_model) != 0) rf_model_reset(&s_model);
#if SIMULACRA_LEARN
    learn_reset();
    learn_load_nvs();          // resume the learned library across reboots (empty if none)
#endif
    s_sweep_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_persist_ctr = 0;
    observe_start_scan();
    ESP_LOGW(TAG, "observe scan start rc=%d", s_scan_rc);
}

void observe_heartbeat(void)
{
    ESP_LOGW(TAG, "alive scan_rc=%d sweeps=%u obs=%u distinct=%u",
             s_scan_rc, (unsigned)s_model.sweeps, (unsigned)s_model.total_obs,
             (unsigned)observe_ephemeral_count());
}

void observe_reprofile_init(uint32_t boot_salt)
{
    observe_reset_ephemeral(boot_salt);                      // sets the per-boot salt
    if (rf_model_load_nvs(&s_model) != 0) rf_model_reset(&s_model);
#if SIMULACRA_LEARN
    learn_reset();
    learn_load_nvs();          // resume the learned library across reboots (empty if none)
#endif
}

void observe_window(uint32_t duration_ms)
{
    observe_reset_ephemeral(s_salt);                         // fresh dedup table, keep the boot salt
    s_window_mode    = true;
    s_sweep_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    observe_start_scan();                                    // EXT_DISC reports -> observe_gap_event
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    int cancel_rc = ble_gap_disc_cancel();
    if (cancel_rc != 0) ESP_LOGW(TAG, "observe_window: disc_cancel rc=%d", cancel_rc);
    vTaskDelay(pdMS_TO_TICKS(50));     // let queued reports drain on the host task before closing the sweep
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    observe_end_sweep(&s_model, now - s_sweep_start_ms);
    s_window_mode = false;
    ESP_LOGW(TAG, "[reprofile sweep %u] pop=%u arr/min=%u obs=%u",
             (unsigned)s_model.sweeps, (unsigned)(s_model.pop_ewma + 0.5f),
             (unsigned)(s_model.arrival_per_min + 0.5f), (unsigned)s_model.total_obs);
}

const rf_model_t *observe_model(void) { return &s_model; }
