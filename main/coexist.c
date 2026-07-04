#include "sdkconfig.h"
#include "coexist.h"

#if CONFIG_IDF_TARGET_ESP32C5
static const coexist_persona_t s_persona = {       // Ward: dense, mains, dual-band, stationary
    .wifi_period_ms      = 2000,                    // heavier Wi-Fi (~2 s)
    .reprofile_period_ms = 600000,                  // 10 min
    .use_5g              = true,
    .drift_threshold     = 2.0f,                    // unreachable -> anti-entourage off
};
#else
static const coexist_persona_t s_persona = {       // Shade: lean, battery, 2.4-only, portable
    .wifi_period_ms      = 7000,                    // thin Wi-Fi (~6-8 s)
    .reprofile_period_ms = 300000,                  // 5 min
    .use_5g              = false,
    .drift_threshold     = 0.45f,                   // active anti-entourage
};
#endif

const coexist_persona_t *coexist_persona(void) { return &s_persona; }

coexist_due_t coexist_due(const coexist_persona_t *p, uint32_t now_ms,
                          uint32_t *last_wifi_ms, uint32_t *last_reprofile_ms)
{
    coexist_due_t d = { false, false };
    if (now_ms - *last_wifi_ms >= p->wifi_period_ms)            { d.fire_wifi = true;      *last_wifi_ms = now_ms; }
    if (now_ms - *last_reprofile_ms >= p->reprofile_period_ms)  { d.fire_reprofile = true; *last_reprofile_ms = now_ms; }
    return d;
}

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "churn.h"
#include "probe.h"
#include "drift.h"
#include "observe.h"
#include "generate.h"
#include "roster.h"
#include "rf_model.h"
#include "esp_random.h"
#include "detect.h"
#include "sig_class_name.h"
#include <string.h>

static const char *TAG = "coexist";
#define OBS_REPROFILE_MS   15000
#define COEX_TICK_MS       250
#define COEX_5G_EVERY      4               // do a 5 GHz excursion every Nth Wi-Fi burst (keep it sparse)
#define SHADE_DRIFT_ACCEL  3.0f
#define SHADE_ACCEL_DECAY_MS 120000u       // ~2 min linear decay back to 1.0

static bool     s_wifi_ok;
static bool     s_wifi_allowed = true;    // webui: false defers Wi-Fi (STA) so the config AP can own it
static uint32_t s_wifi_ctr;
static uint32_t s_accel_until_ms;         // 0 = not accelerating
static int      s_listen_ch = -1;         // espnow: >=0 -> park Wi-Fi on this channel between bursts to listen

// --- M9 detection wiring ---
#define DETECT_EPOCH_DRIFT 0.45f           // detection-owned; separate from anti-entourage thresh
#define COEX_SELF_MAX      16              // max decoy active-set MACs to self-exclude (Ward ceiling)
static uint16_t s_epoch;                   // location-epoch counter
static uint32_t s_detect_salt;             // per-install salt (stable across sweeps/reboots)
static struct { uint32_t hash; int8_t last_rssi; uint32_t last_ms; bool used; }
       s_locate[DETECT_MAX_THREATS];       // per-confirmed-threat locate-throttle state

uint16_t coexist_current_epoch(void) { return s_epoch; }

#if CONFIG_IDF_TARGET_ESP32C5
// C5 hard exclusion: tuning to 5 GHz means BLE (2.4 GHz) cannot TX. Inject a small ROTATING
// subset of the 5 GHz set per excursion (injecting all 8 back-to-back floods the Wi-Fi TX
// buffer -> ESP_ERR_NO_MEM/257 on the later channels), draining between channels, then retune
// to 2.4 GHz so BLE adv resumes. Full 5 GHz coverage rolls over several excursions (still sparse).
#define COEX_5G_PER_EXCURSION 2
static void coexist_5g_excursion(void)
{
    const uint8_t *ch5; size_t n5 = probe_channels_5g(&ch5);
    static size_t idx;
    for (int k = 0; k < COEX_5G_PER_EXCURSION && n5; k++, idx++) {
        if (k) vTaskDelay(pdMS_TO_TICKS(3));   // let the TX buffer drain between channels
        probe_inject_burst(ch5[idx % n5]);
    }
    const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
    if (n24) esp_wifi_set_channel(ch24[0], WIFI_SECOND_CHAN_NONE);   // back to 2.4 GHz
}
#else
static void coexist_5g_excursion(void) {}
#endif

static void coexist_handle_drift(const coexist_persona_t *p, float score)
{
#if !CONFIG_IDF_TARGET_ESP32C5                  // Shade (C6) only; Ward is stationary
    if (drift_exceeds(score, p->drift_threshold)) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        s_accel_until_ms = now + SHADE_ACCEL_DECAY_MS;
        churn_set_accel(SHADE_DRIFT_ACCEL);
        ESP_LOGW(TAG, "anti-entourage: drift=%.3f > %.2f -> accel=%.1f for %ums",
                 score, p->drift_threshold, SHADE_DRIFT_ACCEL, (unsigned)SHADE_ACCEL_DECAY_MS);
    }
#else
    (void)p; (void)score;
#endif
}

// --- M9 detector wiring adapter (impure glue; keeps detect.c pure) ---

// Build the self-exclusion set from the churn active identities (our own live decoy MACs).
static size_t coexist_self_macs(uint8_t out[][6], size_t max)
{
    size_t n = 0;
    for (size_t s = 0; s < churn_active_count() && n < max; s++) {
        const identity_t *id = churn_active_at(s);
        if (id) memcpy(out[n++], id->addr, 6);
    }
    return n;
}

// M9 per-install-salted FNV-1a over the MAC (stable across sweeps/reboots -- deliberate).
static uint32_t coexist_detect_hash(const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ s_detect_salt;
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

static void coexist_locate_emit(uint32_t hash, uint32_t id_prefix, int8_t rssi, uint32_t now_ms)
{
    int slot = -1;
    for (size_t i = 0; i < DETECT_MAX_THREATS; i++) {
        if (s_locate[i].used && s_locate[i].hash == hash) { slot = (int)i; break; }
        if (slot < 0 && !s_locate[i].used) slot = (int)i;
    }
    if (slot < 0) return;
    if (!s_locate[slot].used) {                       // first sighting since confirm -> emit + seed
        s_locate[slot].used = true; s_locate[slot].hash = hash;
        s_locate[slot].last_rssi = rssi; s_locate[slot].last_ms = now_ms;
        ESP_LOGW(TAG, "THREAT locate id=%04x rssi=%d seen=+0s", (unsigned)id_prefix, rssi);
        return;
    }
    if (detect_locate_due(rssi, s_locate[slot].last_rssi, now_ms, s_locate[slot].last_ms)) {
        ESP_LOGW(TAG, "THREAT locate id=%04x rssi=%d seen=+%us", (unsigned)id_prefix, rssi,
                 (unsigned)((now_ms - s_locate[slot].last_ms) / 1000));
        s_locate[slot].last_rssi = rssi; s_locate[slot].last_ms = now_ms;
    }
}

// Registered on observe: fires for every raw report (NimBLE host-task context).
static void coexist_on_report(const uint8_t mac[6], int8_t rssi, uint16_t company,
                              const sig_hit_t *hit)
{
    if (!detect_enabled()) return;
    uint8_t self[COEX_SELF_MAX][6]; size_t nself = coexist_self_macs(self, COEX_SELF_MAX);
    if (detect_mac_in_set(mac, self, nself)) return;        // never flag our own decoys

    uint32_t hash = coexist_detect_hash(mac);
    uint16_t epoch = s_epoch;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (hit) {                                              // M10 fingerprint: known device class
        if (detect_note_known(hash, rssi, hit->class_id, hit->category, hit->confidence, epoch)
                == DETECT_CONFIRM)
            ESP_LOGW(TAG, "KNOWN %s id=%04x conf=%u rssi=%d", sig_class_name(hit->class_id),
                     (unsigned)(hash & 0xFFFF), (unsigned)hit->confidence, rssi);
    }
    detect_result_t r = detect_observe(hash, rssi, company, epoch);
    if (r == DETECT_CONFIRM) {
        ESP_LOGW(TAG, "THREAT confirmed id=%04x vendor=0x%04x epochs=%u rssi=%d "
                      "mac=%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)(hash & 0xFFFF), company, DETECT_EPOCH_STRIKES, rssi,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else if (r == DETECT_KNOWN) {
        coexist_locate_emit(hash, hash & 0xFFFF, rssi, now);
    }
}

// Optional status LED: slow-blink while >=1 confirmed threat is active. Board-gated -- compiled
// out unless SIMULACRA_DETECT_LED_GPIO is defined (LED wiring differs across boards).
#ifdef SIMULACRA_DETECT_LED_GPIO
#include "driver/gpio.h"
static void coexist_detect_led_init(void)
{
    gpio_reset_pin((gpio_num_t)SIMULACRA_DETECT_LED_GPIO);
    gpio_set_direction((gpio_num_t)SIMULACRA_DETECT_LED_GPIO, GPIO_MODE_OUTPUT);
}
static void coexist_detect_led_tick(uint32_t now_ms)
{
    static uint32_t last; static int on;
    if (detect_threat_count() == 0) { gpio_set_level((gpio_num_t)SIMULACRA_DETECT_LED_GPIO, 0); return; }
    if (now_ms - last >= 500) { on = !on; last = now_ms;                 // slow blink = threat active
        gpio_set_level((gpio_num_t)SIMULACRA_DETECT_LED_GPIO, on); }
}
#else
static inline void coexist_detect_led_init(void) {}
static inline void coexist_detect_led_tick(uint32_t now_ms) { (void)now_ms; }
#endif

static void coexist_reprofile(const coexist_persona_t *p)
{
    rf_model_t prev = *observe_model();                     // snapshot pre-update
    observe_window(OBS_REPROFILE_MS);                       // ~15 s scan while advertising
    const rf_model_t *cur = observe_model();
    if (cur->total_obs < GEN_MIN_OBS) {                     // too sparse -> keep current population
        ESP_LOGW(TAG, "reprofile: total_obs=%u < %d -> skip reshape",
                 (unsigned)cur->total_obs, GEN_MIN_OBS);
        return;
    }
    float score = drift_score(&prev, cur);
    if (prev.sweeps > 0 && score > DETECT_EPOCH_DRIFT) {    // materially new room -> new location-epoch
        s_epoch++;
        detect_on_epoch_change(s_epoch);
        ESP_LOGW(TAG, "epoch -> %u (drift=%.3f)", (unsigned)s_epoch, score);
    }
    roster_reseed_idle(cur);                                // fresh identities into the IDLE pool
    uint8_t at = generate_active_target(cur);
    churn_set_active_target(at);                            // resize to the new population
    ESP_LOGW(TAG, "reprofile: drift=%.3f active_target=%u", score, (unsigned)at);
    if (prev.sweeps > 0) coexist_handle_drift(p, score);   // skip day-one false trigger (empty prev model)
}

static void coexist_decay_accel(void)
{
    if (s_accel_until_ms == 0) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now >= s_accel_until_ms) {
        churn_set_accel(1.0f);
        s_accel_until_ms = 0;
        ESP_LOGW(TAG, "anti-entourage: accel decayed to 1.0");
        return;
    }
    float frac = (float)(s_accel_until_ms - now) / (float)SHADE_ACCEL_DECAY_MS;   // 1.0 -> 0
    churn_set_accel(1.0f + (SHADE_DRIFT_ACCEL - 1.0f) * frac);                    // linear decay
}

static void coexist_task(void *arg)
{
    (void)arg;
    const coexist_persona_t *p = coexist_persona();
    uint32_t now0 = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_wifi = now0, last_repro = now0;      // don't fire at the instant of boot
    uint32_t hop24 = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        churn_tick(now);                                // BLE ext-adv runs continuously under coex
        coexist_decay_accel();
        detect_threat_t nt;
        if (detect_drain_pending(&nt)) {                // persist a new confirmation off the BLE callback
            int rc = detect_save_nvs();
            ESP_LOGW(TAG, "THREAT persisted id=%04x rc=%d", (unsigned)(nt.hash & 0xFFFF), rc);
        }
        coexist_detect_led_tick(now);
        coexist_due_t d = coexist_due(p, now, &last_wifi, &last_repro);
        if (d.fire_wifi && s_wifi_ok) {
            const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
            if (p->use_5g && (++s_wifi_ctr % COEX_5G_EVERY == 0)) coexist_5g_excursion();
        }
        if (d.fire_reprofile) coexist_reprofile(p);
        if (s_listen_ch >= 0 && s_wifi_ok)                       // espnow: park on the listen channel between bursts
            esp_wifi_set_channel((uint8_t)s_listen_ch, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(COEX_TICK_MS));
    }
}

void coexist_set_listen_channel(int ch)
{
    s_listen_ch = ch;      // -1 disables; >=0 makes the coexist tick return the radio here between bursts
}

void coexist_set_wifi_enabled(bool en)
{
    if (en && !s_wifi_ok) {                       // bring Wi-Fi (STA) up now that the AP is down
        int rc = probe_wifi_init();
        s_wifi_ok = (rc == 0);
        if (s_wifi_ok) probe_pool_init();
        ESP_LOGW(TAG, "coexist: wifi enabled post-config rc=%d", rc);
    }
    s_wifi_allowed = en;
}

void coexist_start(void)
{
    if (s_wifi_allowed) {
        int rc = probe_wifi_init();
        s_wifi_ok = (rc == 0);
        if (s_wifi_ok) { probe_pool_init(); ESP_LOGW(TAG, "coexist: wifi up -> BLE + Wi-Fi combined decoy"); }
        else           { ESP_LOGE(TAG, "coexist: wifi init rc=%d -> BLE-only fallback", rc); }
    } else {
        s_wifi_ok = false;
        ESP_LOGW(TAG, "coexist: wifi deferred (config window) -> BLE-only for now");
    }
    observe_reprofile_init(esp_random());
    s_detect_salt = detect_load_salt();          // M9: stable per-install salt
    detect_load_nvs();                            // restore previously-confirmed threats (best-effort)
    observe_set_report_cb(coexist_on_report);     // subscribe the detector to raw reports
    coexist_detect_led_init();
    BaseType_t ok = xTaskCreate(coexist_task, "coexist", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "coexist_task create failed -> BLE-only emergency loop");
        for (;;) {                    // never brick: keep the BLE decoy advertising
            churn_tick((uint32_t)(esp_timer_get_time() / 1000));
            vTaskDelay(pdMS_TO_TICKS(COEX_TICK_MS));
        }
    }
}
