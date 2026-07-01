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

static const char *TAG = "coexist";
#define OBS_REPROFILE_MS   15000
#define COEX_TICK_MS       250
#define COEX_5G_EVERY      4               // do a 5 GHz excursion every Nth Wi-Fi burst (keep it sparse)
#define SHADE_DRIFT_ACCEL  3.0f
#define SHADE_ACCEL_DECAY_MS 120000u       // ~2 min linear decay back to 1.0

static bool     s_wifi_ok;
static uint32_t s_wifi_ctr;
static uint32_t s_accel_until_ms;         // 0 = not accelerating

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
        coexist_due_t d = coexist_due(p, now, &last_wifi, &last_repro);
        if (d.fire_wifi && s_wifi_ok) {
            const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
            if (p->use_5g && (++s_wifi_ctr % COEX_5G_EVERY == 0)) coexist_5g_excursion();
        }
        if (d.fire_reprofile) coexist_reprofile(p);
        vTaskDelay(pdMS_TO_TICKS(COEX_TICK_MS));
    }
}

void coexist_start(void)
{
    int rc = probe_wifi_init();
    s_wifi_ok = (rc == 0);
    if (s_wifi_ok) { probe_pool_init(); ESP_LOGW(TAG, "coexist: wifi up -> BLE + Wi-Fi combined decoy"); }
    else           { ESP_LOGE(TAG, "coexist: wifi init rc=%d -> BLE-only fallback", rc); }
    observe_reprofile_init(esp_random());
    BaseType_t ok = xTaskCreate(coexist_task, "coexist", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "coexist_task create failed -> BLE-only emergency loop");
        for (;;) {                    // never brick: keep the BLE decoy advertising
            churn_tick((uint32_t)(esp_timer_get_time() / 1000));
            vTaskDelay(pdMS_TO_TICKS(COEX_TICK_MS));
        }
    }
}
