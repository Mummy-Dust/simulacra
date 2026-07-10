#include "sniff.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sniff";
static volatile uint32_t s_probes;   // total probe requests seen
static volatile uint32_t s_rand;     // those with a locally-administered (randomized) source MAC
static const uint8_t CH[] = { 1, 6, 11 };

// Debug: park on a single 2.4 GHz channel instead of hopping (0 = hop). Used to isolate an
// injector pinned to the same channel from ambient noise.
#define SNIFF_FIXED_CH 1

// Per-frame SA+seq logging: verifies the injector's 802.11 sequence numbers are independent
// per source MAC (not one shared hardware counter). 0 = counts only.
#define SNIFF_LOG_FRAMES 1

static void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    if (p->rx_ctrl.sig_len < 24) return;
    const uint8_t *f = p->payload;
    if (f[0] != 0x40) return;            // frame control: probe request
    s_probes++;
    if (f[10] & 0x02) s_rand++;          // source MAC (offset 10) locally-administered
#if SNIFF_LOG_FRAMES
    uint16_t seq = (uint16_t)((f[22] | (f[23] << 8)) >> 4);   // 802.11 sequence number (12 bits)
    ESP_LOGW(TAG, "pr sa=%02x:%02x:%02x:%02x:%02x:%02x seq=%u rssi=%d",
             f[10], f[11], f[12], f[13], f[14], f[15], (unsigned)seq, p->rx_ctrl.rssi);
#endif
}

static void sniff_task(void *arg)
{
    (void)arg;
#if SNIFF_FIXED_CH
    esp_wifi_set_channel(SNIFF_FIXED_CH, WIFI_SECOND_CHAN_NONE);   // parked (A/B isolation test)
    (void)CH;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        ESP_LOGW(TAG, "probe-reqs seen=%u (randomized-MAC=%u) [parked ch%d]",
                 (unsigned)s_probes, (unsigned)s_rand, SNIFF_FIXED_CH);
    }
#else
    int h = 0;
    for (;;) {
        esp_wifi_set_channel(CH[h % (int)(sizeof(CH) / sizeof(CH[0]))], WIFI_SECOND_CHAN_NONE);
        h++;
        vTaskDelay(pdMS_TO_TICKS(300));
        if (h % 10 == 0)
            ESP_LOGW(TAG, "probe-reqs seen=%u (randomized-MAC=%u)", (unsigned)s_probes, (unsigned)s_rand);
    }
#endif
}

void sniff_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(rx_cb);
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    xTaskCreate(sniff_task, "sniff", 4096, NULL, 5, NULL);
    ESP_LOGW(TAG, "wifi probe sniffer started (2.4 GHz hop 1/6/11)");
}
