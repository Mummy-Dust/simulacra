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
#define PROBE_PHONES   16
#define PROBE_USE_5G   1
#else
#define PROBE_PHONES   8
#define PROBE_USE_5G   0
#endif
#define PROBE_MAX_PHONES 16
#define PROBE_BURST_MS   2000
// TX health: this many CONSECUTIVE esp_wifi_80211_tx failures means the decoy is alive but not
// actually injecting (radio/driver wedged) -> report degraded. A single success clears it.
#define PROBE_TX_FAIL_THRESH 16

// --- en_sys_seq bench-gate hooks (default-off; see tools/seq_gate). Shipped decoy UNAFFECTED. ---
// The gate needs single-channel injection so the sniffer hears EVERY frame (a shared HW counter
// ticks on every TX regardless of channel; a hopping injector viewed on one channel hides the
// +1 signature). PROBE_FORCE_SHARED deliberately triggers the regression to prove the gate catches it.
#ifndef PROBE_FIX_CH
#define PROBE_FIX_CH 0        // >0: pin injection to this channel; 0 = normal 1/6/11(+5G) hop
#endif
#ifndef PROBE_FORCE_SHARED
#define PROBE_FORCE_SHARED 0  // 1: en_sys_seq=true (shared HW counter) — regression simulation only
#endif

static const uint8_t CH_24[] = { 1, 6, 11 };
#if PROBE_USE_5G
static const uint8_t CH_5[]  = { 36, 40, 44, 48, 149, 153, 157, 161 };
#endif

static int      s_hop;
static uint32_t s_probes_sent;   // cumulative probe requests injected (dashboard telemetry)
static uint16_t s_tx_consec_fail; // consecutive TX failures (reset on any success) -> health flag

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
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    probe_agents_init(PROBE_PHONES, now);
    ESP_LOGW(TAG, "probe agents: %d (independent seq, jittered scan, lifecycle churn)",
             probe_agents_count());
}

int probe_phone_count(void) { return probe_agents_count(); }

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
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    probe_agents_lifecycle(now);                                // birth/death turnover
    int crc = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    probe_agent_t *due[PROBE_MAX_PHONES];
    int nd = probe_agents_due(now, due, PROBE_MAX_PHONES);      // decorrelated partial subset
    int rc = 0, sent = 0;
    for (int i = 0; i < nd; i++) {
        uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
        if (probe_build_request(due[i]->mac, channel, due[i]->arch, band5, f, &n) != 0)
            continue;                                           // archetype lacks this band (defensive)
        uint16_t sc = (uint16_t)(probe_agent_next_seq(due[i]) << 4);   // seq -> bits 4..15, frag=0
        f[22] = (uint8_t)(sc & 0xFF);
        f[23] = (uint8_t)((sc >> 8) & 0xFF);
        // en_sys_seq=false honors the per-agent seq we wrote (the whole defense). The gate can force
        // true (PROBE_FORCE_SHARED) to simulate the shared-HW-counter regression and prove it's caught.
        rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, PROBE_FORCE_SHARED);  // 0 -> false (shipped)
        if (rc != 0) { if (s_tx_consec_fail < 0xFFFF) s_tx_consec_fail++; }
        else s_tx_consec_fail = 0;
        s_probes_sent++; sent++;
    }
    ESP_LOGW(TAG, "burst ch=%u due=%d/%d band5=%d set_ch_rc=%d tx_rc=%d",
             channel, sent, probe_agents_count(), band5, crc, rc);
    return rc;
}

uint32_t probe_total_sent(void) { return s_probes_sent; }

// TX health for the dashboard: false once PROBE_TX_FAIL_THRESH sends in a row have failed (the
// decoy reports fine but isn't injecting). Clears on the next successful TX. Catches a wedged
// radio/driver -- NOT the en_sys_seq regression (successful TX with a bad seq; that needs a sniffer).
bool probe_tx_healthy(void) { return s_tx_consec_fail < PROBE_TX_FAIL_THRESH; }

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
#if PROBE_FIX_CH
        probe_inject_burst(PROBE_FIX_CH);   // gate mode: single channel so the sniffer hears every frame
#else
        probe_inject_burst(next_channel());
#endif
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
