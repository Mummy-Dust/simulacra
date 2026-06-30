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

static const char *TAG = "coexist";
#define COEX_TICK_MS   250
#define COEX_5G_EVERY  4               // do a 5 GHz excursion every Nth Wi-Fi burst (keep it sparse)

static bool     s_wifi_ok;
static uint32_t s_wifi_ctr;

#if CONFIG_IDF_TARGET_ESP32C5
// C5 hard exclusion: tuning to 5 GHz means BLE (2.4 GHz) cannot TX. Batch a quick sweep
// across the 5 GHz set, then immediately retune to 2.4 GHz so BLE adv resumes.
static void coexist_5g_excursion(void)
{
    const uint8_t *ch5; size_t n5 = probe_channels_5g(&ch5);
    for (size_t i = 0; i < n5; i++) probe_inject_burst(ch5[i]);
    const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
    if (n24) esp_wifi_set_channel(ch24[0], WIFI_SECOND_CHAN_NONE);   // back to 2.4 GHz
}
#else
static void coexist_5g_excursion(void) {}
#endif

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
        coexist_due_t d = coexist_due(p, now, &last_wifi, &last_repro);
        if (d.fire_wifi && s_wifi_ok) {
            const uint8_t *ch24; size_t n24 = probe_channels_24(&ch24);
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
            if (p->use_5g && (++s_wifi_ctr % COEX_5G_EVERY == 0)) coexist_5g_excursion();
        }
        if (d.fire_reprofile) {
            // Layer 2 (Task 6) hooks in here. Layer 1: heartbeat only.
            ESP_LOGW(TAG, "reprofile slot (Layer 2 lands in Task 6)");
        }
        vTaskDelay(pdMS_TO_TICKS(COEX_TICK_MS));
    }
}

void coexist_start(void)
{
    int rc = probe_wifi_init();
    s_wifi_ok = (rc == 0);
    if (s_wifi_ok) { probe_pool_init(); ESP_LOGW(TAG, "coexist: wifi up -> BLE + Wi-Fi combined decoy"); }
    else           { ESP_LOGE(TAG, "coexist: wifi init rc=%d -> BLE-only fallback", rc); }
    xTaskCreate(coexist_task, "coexist", 8192, NULL, 5, NULL);
}
