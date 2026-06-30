#include <string.h>
#include "observe.h"
#include "esp_log.h"
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
    memset(s_tbl, 0, sizeof(s_tbl));       // wipe ephemeral identifiers
    s_arrivals = 0;
    s_saturated = false;
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
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 &&
        f.mfg_data && f.mfg_data_len >= 2)
        company = (uint16_t)(f.mfg_data[0] | (f.mfg_data[1] << 8));

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // legacy_event_type is the PDU type for legacy ads; non-legacy ext ads clamp to the last bin.
    observe_ingest(&s_model, d->addr.val, now, company, d->rssi, d->legacy_event_type);
    if (!s_window_mode) observe_maybe_close_sweep(now);      // window mode closes explicitly
    return 0;
}

void observe_start(uint32_t boot_salt)
{
    observe_reset_ephemeral(boot_salt);
    if (rf_model_load_nvs(&s_model) != 0) rf_model_reset(&s_model);
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
}

void observe_window(uint32_t duration_ms)
{
    observe_reset_ephemeral(s_salt);                         // fresh dedup table, keep the boot salt
    s_window_mode    = true;
    s_sweep_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    observe_start_scan();                                    // EXT_DISC reports -> observe_gap_event
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ble_gap_disc_cancel();                                   // stop scanning; advertising continues
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    observe_end_sweep(&s_model, now - s_sweep_start_ms);
    s_window_mode = false;
    ESP_LOGW(TAG, "[reprofile sweep %u] pop=%u arr/min=%u obs=%u",
             (unsigned)s_model.sweeps, (unsigned)(s_model.pop_ewma + 0.5f),
             (unsigned)(s_model.arrival_per_min + 0.5f), (unsigned)s_model.total_obs);
}

const rf_model_t *observe_model(void) { return &s_model; }
