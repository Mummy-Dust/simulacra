#include "esp_now_link.h"
#include <string.h>

void espnow_status_from_webui(radar_wire_status_t *out, const webui_status_t *in)
{
    memset(out, 0, sizeof(*out));
    out->uptime_s = in->uptime_s;
    out->flags = (uint8_t)((in->decoy_paused ? 0x1 : 0) | (in->wifi_config_mode ? 0x2 : 0));
    out->active_devices = in->active_devices; out->roster_size = in->roster_size;
    out->probes_sent = in->probes_sent; out->epoch = in->epoch; out->pop_ewma = in->pop_ewma;
    out->total_obs = in->total_obs; out->active_target = in->active_target;
    uint8_t n = in->threat_count; if (n > RADAR_MAX_THREATS) n = RADAR_MAX_THREATS;
    out->threat_count = n;
    for (uint8_t i = 0; i < n; i++) {
        out->threats[i].hash = in->threats[i].hash;
        out->threats[i].vendor = in->threats[i].vendor;
        out->threats[i].epochs = in->threats[i].epochs;
        out->threats[i].best_rssi = in->threats[i].best_rssi;
        out->threats[i].first_epoch = in->threats[i].first_epoch;
        out->threats[i].last_epoch = in->threats[i].last_epoch;
    }
}

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radar_key.h"
#include "coexist.h"

#ifndef SIMULACRA_ESPNOW_CHANNEL
#define SIMULACRA_ESPNOW_CHANNEL 1
#endif
static const char *ETAG = "espnow";
static const uint8_t BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static uint8_t   s_salt[4];
static uint64_t  s_counter;
static radar_replay_t s_req_replay;                 // reject replayed requests
static volatile bool  s_answer;                     // set by RX cb, consumed by task

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    uint8_t type, pl[RADAR_FRAME_MAX], salt[4]; size_t plen; uint64_t ctr;
    if (radar_wire_open(data, (size_t)len, SIMULACRA_ESPNOW_KEY, &type, pl, &plen, salt, &ctr) != 0)
        return;                                     // not ours / bad tag
    if (type != RADAR_TYPE_REQUEST) return;
    if (!radar_replay_ok(&s_req_replay, salt, ctr)) return;   // replayed request
    s_answer = true;                                // defer the heavy work to the task
}

static void respond_once(void)
{
    webui_status_t w; webui_gather_status(&w);
    radar_wire_status_t r; espnow_status_from_webui(&r, &w);
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    if (radar_wire_seal(frame, &flen, RADAR_TYPE_STATUS, (uint8_t*)&r, sizeof r,
                        SIMULACRA_ESPNOW_KEY, s_salt, ++s_counter) != 0) return;
    for (int i = 0; i < 3; i++) esp_now_send(BCAST, frame, flen);   // 3x back-to-back
    ESP_LOGW(ETAG, "answered request (%u B x3)", (unsigned)flen);
}

static void espnow_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_answer) { s_answer = false; respond_once(); }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void esp_now_link_start(void)
{
    // Wi-Fi is already up (coexist STA). Randomize the STA source MAC once (locally-administered).
    uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_fill_random(mac, 6); mac[0] = (mac[0] & 0xFE) | 0x02;      // LAA, unicast
    esp_wifi_set_mac(WIFI_IF_STA, mac);                            // best-effort; ignore rc
    esp_fill_random(s_salt, sizeof s_salt);

    if (esp_now_init() != ESP_OK) { ESP_LOGE(ETAG, "esp_now_init failed"); return; }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BCAST, 6); peer.channel = SIMULACRA_ESPNOW_CHANNEL; peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
    esp_now_register_recv_cb(on_recv);
    coexist_set_listen_channel(SIMULACRA_ESPNOW_CHANNEL);  // park the radio on ch1 between probe bursts
    xTaskCreate(espnow_task, "espnow", 4096, NULL, 3, NULL);
    ESP_LOGW(ETAG, "responder up (ch=%d, listen-only until requested)", SIMULACRA_ESPNOW_CHANNEL);
}
