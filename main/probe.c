#include <string.h>
#include "probe.h"
#include "esp_random.h"

void probe_random_mac(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[0] = (uint8_t)((out[0] & 0xFC) | 0x02);   // locally-administered, unicast
        int zero = 1, ff = 1;
        for (int i = 0; i < 6; i++) { if (out[i]) zero = 0; if (out[i] != 0xff) ff = 0; }
        if (!zero && !ff) return;
    }
}

int probe_build_request(const uint8_t mac[6], uint8_t channel, uint8_t *out, size_t *out_len)
{
    static const uint8_t hdr_tail[] = {
        0x00, 0x00,                         // SSID IE: id 0, len 0  (WILDCARD -- Law 3)
        0x01, 0x04, 0x02, 0x04, 0x0b, 0x16, // Supported Rates: 1,2,5.5,11 Mbps
        0x32, 0x04, 0x0c, 0x12, 0x18, 0x24, // Extended Supported Rates: 6,9,12,18 Mbps
        0x03, 0x01, 0x00,                   // DS Parameter Set: id 3, len 1, channel (filled below)
    };
    uint8_t *p = out;
    *p++ = 0x40; *p++ = 0x00;               // frame control: mgmt/probe-req, no flags
    *p++ = 0x00; *p++ = 0x00;               // duration
    memset(p, 0xff, 6); p += 6;             // DA broadcast
    memcpy(p, mac, 6); p += 6;              // SA = our randomized MAC
    memset(p, 0xff, 6); p += 6;             // BSSID broadcast
    *p++ = 0x00; *p++ = 0x00;               // seq control (driver overwrites when en_sys_seq=true)
    memcpy(p, hdr_tail, sizeof(hdr_tail)); p += sizeof(hdr_tail);
    out[p - out - 1] = channel;             // DS param channel = last byte
    *out_len = (size_t)(p - out);
    return 0;
}

// --- live radio path ---

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
#define PROBE_ROTATE_MS  30000

static const uint8_t CH_24[] = { 1, 6, 11 };
#if PROBE_USE_5G
static const uint8_t CH_5[]  = { 36, 40, 44, 48, 149, 153, 157, 161 };
#endif

static uint8_t s_macs[PROBE_MAX_PHONES][6];
static int     s_n;
static int     s_hop;

// Debug: pin injection to a single 2.4 GHz channel (0 = normal hop). For A/B isolation tests.
#define PROBE_PIN_CH 0

static uint8_t next_channel(void)
{
#if PROBE_PIN_CH
    return PROBE_PIN_CH;
#endif
    // interleave 2.4 GHz with 5 GHz (C5) so both bands get traffic
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
    s_n = PROBE_PHONES; if (s_n > PROBE_MAX_PHONES) s_n = PROBE_MAX_PHONES;
    for (int i = 0; i < s_n; i++) probe_random_mac(s_macs[i]);
    uint32_t last_rotate = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t ch = next_channel();
        int crc = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        int rc = 0;
        for (int i = 0; i < s_n; i++) {
            uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
            probe_build_request(s_macs[i], ch, f, &n);
            rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, true);
        }
        if (now - last_rotate >= PROBE_ROTATE_MS) {   // retire one fake phone -> fresh MAC
            last_rotate = now;
            probe_random_mac(s_macs[esp_random() % s_n]);
        }
        ESP_LOGW(TAG, "burst ch=%u phones=%d set_ch_rc=%d tx_rc=%d", ch, s_n, crc, rc);
        vTaskDelay(pdMS_TO_TICKS(PROBE_BURST_MS));
    }
}

void probe_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#if CONFIG_IDF_TARGET_ESP32C5
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);   // 2.4 + 5 GHz
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(probe_task, "probe", 4096, NULL, 5, NULL);
    ESP_LOGW(TAG, "probe injector started (phones=%d 5g=%d)", PROBE_PHONES, PROBE_USE_5G);
}
