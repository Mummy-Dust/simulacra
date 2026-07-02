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
