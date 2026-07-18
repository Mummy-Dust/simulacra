#include "wifi_observe.h"
#include "wifi_density.h"
#include "fleet.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "wifiobs";

static void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    if (p->rx_ctrl.sig_len < 24) return;
    const uint8_t *f = p->payload;
    if (f[0] != 0x40) return;                 // frame control: probe request
    const uint8_t *sa = f + 10;               // source MAC
    if (!(sa[0] & 0x02)) return;              // randomized (locally-administered) only = real-phone proxy
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (fleet_mac_excluded(sa, now)) return;  // skip fleetmate decoys (our own are never received)
    wifi_obs_note(sa, now);                   // raw MAC hashed-and-dropped inside
}

bool wifi_obs_start(void)
{
    wifi_obs_reset((uint32_t)esp_random());
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    if (esp_wifi_set_promiscuous_filter(&filt) != ESP_OK) { ESP_LOGW(TAG, "filter set failed -> fallback"); return false; }
    if (esp_wifi_set_promiscuous_rx_cb(rx_cb)  != ESP_OK) { ESP_LOGW(TAG, "rx cb set failed -> fallback");  return false; }
    if (esp_wifi_set_promiscuous(true)         != ESP_OK) { ESP_LOGW(TAG, "promiscuous enable failed -> fallback"); return false; }
    ESP_LOGW(TAG, "wifi observe up (promiscuous on STA)");
    return true;
}
