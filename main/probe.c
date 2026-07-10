// Live radio path. The pure frame builder + archetype tables live in probe_frame.c
// (host-testable); this file owns Wi-Fi bring-up, the fake-phone pool, and injection.
#include "probe.h"
#include "esp_random.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "probe";

#if CONFIG_IDF_TARGET_ESP32C5
#define PROBE_PHONES   8
#define PROBE_USE_5G   1
#else
#define PROBE_PHONES   4
#define PROBE_USE_5G   0
#endif
#define PROBE_MAX_PHONES 12
#define PROBE_BURST_MS   2000
#define PROBE_ROTATE_EVERY 15          // ~1-in-15 bursts rotates one fake phone to a fresh MAC

static const uint8_t CH_24[] = { 1, 6, 11 };
#if PROBE_USE_5G
static const uint8_t CH_5[]  = { 36, 40, 44, 48, 149, 153, 157, 161 };
#endif

typedef struct { uint8_t mac[6]; probe_arch_t arch; } probe_phone_t;
static probe_phone_t s_phones[PROBE_MAX_PHONES];
static int      s_n;
static int      s_hop;
static uint32_t s_probes_sent;   // cumulative probe requests injected (dashboard telemetry)

int probe_wifi_init(void)
{
    esp_err_t e;
    if ((e = esp_netif_init()) != ESP_OK) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;   // tolerate pre-existing loop
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) return e;
    if ((e = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return e;
    if ((e = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return e;
#if CONFIG_IDF_TARGET_ESP32C5
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);               // 2.4 + 5 GHz
#endif
    if ((e = esp_wifi_start()) != ESP_OK) return e;
    return 0;
}

void probe_pool_init(void)
{
    s_n = PROBE_PHONES; if (s_n > PROBE_MAX_PHONES) s_n = PROBE_MAX_PHONES;
    int mix[PROBE_ARCH_COUNT] = {0};
    for (int i = 0; i < s_n; i++) {
        probe_random_mac(s_phones[i].mac);
        s_phones[i].arch = probe_pick_archetype();
        mix[s_phones[i].arch]++;
    }
    ESP_LOGW(TAG, "probe pool: %d phones (iphone=%d galaxy=%d pixel=%d android=%d)",
             s_n, mix[ARCH_IPHONE], mix[ARCH_GALAXY], mix[ARCH_PIXEL], mix[ARCH_ANDROID]);
}

int probe_phone_count(void) { return s_n; }

size_t probe_channels_24(const uint8_t **out) { *out = CH_24; return sizeof(CH_24)/sizeof(CH_24[0]); }
size_t probe_channels_5g(const uint8_t **out)
{
#if PROBE_USE_5G
    *out = CH_5; return sizeof(CH_5)/sizeof(CH_5[0]);
#else
    *out = NULL; return 0;
#endif
}

int probe_inject_burst(uint8_t channel)
{
    bool band5 = (channel >= 36);
    int crc = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    int rc = 0;
    for (int i = 0; i < s_n; i++) {
        uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
        if (probe_build_request(s_phones[i].mac, channel, s_phones[i].arch, band5, f, &n) != 0)
            continue;                                           // archetype lacks this band (defensive)
        rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, true);
        s_probes_sent++;
    }
    if ((esp_random() % PROBE_ROTATE_EVERY) == 0) {             // retire one fake phone -> fresh MAC + arch
        int k = esp_random() % s_n;
        probe_random_mac(s_phones[k].mac);
        s_phones[k].arch = probe_pick_archetype();
    }
    ESP_LOGW(TAG, "burst ch=%u phones=%d band5=%d set_ch_rc=%d tx_rc=%d",
             channel, s_n, band5, crc, rc);
    return rc;
}

uint32_t probe_total_sent(void) { return s_probes_sent; }

// Dev mode (SIMULACRA_PROBE): forever-loop wrapper over the scheduler core.
static uint8_t next_channel(void)
{
    s_hop++;
#if PROBE_USE_5G
    if (s_hop & 1) return CH_5[(s_hop / 2) % (int)(sizeof(CH_5) / sizeof(CH_5[0]))];
    return CH_24[(s_hop / 2) % (int)(sizeof(CH_24) / sizeof(CH_24[0]))];
#else
    return CH_24[s_hop % (int)(sizeof(CH_24) / sizeof(CH_24[0]))];
#endif
}

static void probe_task(void *arg)
{
    (void)arg;
    for (;;) {
        probe_inject_burst(next_channel());
        vTaskDelay(pdMS_TO_TICKS(PROBE_BURST_MS));
    }
}

void probe_start(void)
{
    if (probe_wifi_init() != 0) { ESP_LOGE(TAG, "wifi init failed"); return; }
    probe_pool_init();
    xTaskCreate(probe_task, "probe", 4096, NULL, 5, NULL);
    ESP_LOGW(TAG, "probe injector started (phones=%d 5g=%d)", PROBE_PHONES, PROBE_USE_5G);
}
