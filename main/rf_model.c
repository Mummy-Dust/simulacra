#include <string.h>
#include "rf_model.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "rf_model";

void rf_model_reset(rf_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->magic = RF_MODEL_MAGIC;
    m->version = RF_MODEL_VERSION;
}

size_t rf_itvl_bin(int32_t ms)
{
    if (ms < 50)   return 0;
    if (ms < 100)  return 1;
    if (ms < 200)  return 2;
    if (ms < 500)  return 3;
    if (ms < 1000) return 4;
    if (ms < 2000) return 5;
    return 6;
}

size_t rf_rssi_bin(int8_t rssi)
{
    int idx = (rssi + 100) / 10;          // -100 -> 0, -20 -> 8
    if (idx < 0) idx = 0;
    if (idx >= RF_RSSI_BINS) idx = RF_RSSI_BINS - 1;
    return (size_t)idx;
}

size_t rf_pdu_bin(uint8_t pdu_type)
{
    return (pdu_type < RF_PDU_BINS) ? pdu_type : (RF_PDU_BINS - 1);
}

int rf_vendor_index(const rf_model_t *m, uint16_t company_id)
{
    for (size_t i = 0; i < RF_VENDOR_SLOTS; i++)
        if (m->vendors[i].count > 0 && m->vendors[i].company_id == company_id) return (int)i;
    return -1;
}

static uint32_t decayed(uint32_t x)             // subtract 1/DEN, but always at least 1 so small
{ uint32_t d = x / RF_DECAY_DEN; if (d == 0) d = 1; return (x > d) ? x - d : 0; }  // counts eventually reclaim

void rf_model_decay(rf_model_t *m)
{
    for (size_t i = 0; i < RF_VENDOR_SLOTS; i++) {
        rf_vendor_t *v = &m->vendors[i];
        if (v->count == 0) continue;
        v->count = decayed(v->count);
        for (size_t b = 0; b < RF_ITVL_BINS; b++) v->itvl_bins[b] -= v->itvl_bins[b] / RF_DECAY_DEN;
        if (v->count == 0) { v->company_id = 0; memset(v->itvl_bins, 0, sizeof v->itvl_bins); }  // free the slot
    }
    if (m->other_count) m->other_count = decayed(m->other_count);
    for (size_t b = 0; b < RF_ITVL_BINS; b++) m->other_itvl_bins[b] -= m->other_itvl_bins[b] / RF_DECAY_DEN;
    for (size_t b = 0; b < RF_RSSI_BINS; b++) m->rssi_bins[b] -= m->rssi_bins[b] / RF_DECAY_DEN;
    for (size_t b = 0; b < RF_PDU_BINS; b++)  m->pdu_bins[b]  -= m->pdu_bins[b]  / RF_DECAY_DEN;
}

void rf_model_observe(rf_model_t *m, uint16_t company_id, int8_t rssi,
                      uint8_t pdu_type, int32_t interval_ms)
{
    m->total_obs++;
    m->rssi_bins[rf_rssi_bin(rssi)]++;
    m->pdu_bins[rf_pdu_bin(pdu_type)]++;

    int vi = rf_vendor_index(m, company_id);
    if (vi < 0) {                          // claim a free slot if any
        for (size_t i = 0; i < RF_VENDOR_SLOTS; i++) {
            if (m->vendors[i].count == 0) { m->vendors[i].company_id = company_id; vi = (int)i; break; }
        }
    }
    if (vi >= 0) {
        m->vendors[vi].count++;
        if (interval_ms >= 0) m->vendors[vi].itvl_bins[rf_itvl_bin(interval_ms)]++;
    } else {                               // table full -> overflow bucket
        m->other_count++;
        if (interval_ms >= 0) m->other_itvl_bins[rf_itvl_bin(interval_ms)]++;
    }
}

void rf_model_end_sweep(rf_model_t *m, uint32_t distinct_devices, uint32_t window_ms,
                        uint32_t arrivals)
{
    m->sweeps++;
    float pop = (float)distinct_devices;
    float arr = window_ms ? ((float)arrivals * 60000.0f / (float)window_ms) : 0.0f;
    const float a = 0.3f;                  // EWMA weight
    if (m->sweeps == 1) { m->pop_ewma = pop; m->arrival_per_min = arr; }
    else {
        m->pop_ewma        = a * pop + (1.0f - a) * m->pop_ewma;
        m->arrival_per_min = a * arr + (1.0f - a) * m->arrival_per_min;
    }
}

void rf_model_dump(const rf_model_t *m)
{
    // Print floats as rounded integers -- avoids the newlib-nano "%f" pitfall on the C6.
    ESP_LOGW(TAG, "MODEL v%u sweeps=%u obs=%u pop=%u arr/min=%u other=%u",
             m->version, (unsigned)m->sweeps, (unsigned)m->total_obs,
             (unsigned)(m->pop_ewma + 0.5f), (unsigned)(m->arrival_per_min + 0.5f),
             (unsigned)m->other_count);
    for (size_t i = 0; i < RF_VENDOR_SLOTS; i++) {
        if (m->vendors[i].count == 0) continue;
        const rf_vendor_t *v = &m->vendors[i];
        ESP_LOGW(TAG, "  vendor 0x%04X n=%u itvl[%u %u %u %u %u %u %u]",
                 v->company_id, (unsigned)v->count,
                 (unsigned)v->itvl_bins[0], (unsigned)v->itvl_bins[1], (unsigned)v->itvl_bins[2],
                 (unsigned)v->itvl_bins[3], (unsigned)v->itvl_bins[4], (unsigned)v->itvl_bins[5],
                 (unsigned)v->itvl_bins[6]);
    }
    ESP_LOGW(TAG, "  rssi[%u %u %u %u %u %u %u %u] pdu[%u %u %u %u %u]",
             (unsigned)m->rssi_bins[0], (unsigned)m->rssi_bins[1], (unsigned)m->rssi_bins[2],
             (unsigned)m->rssi_bins[3], (unsigned)m->rssi_bins[4], (unsigned)m->rssi_bins[5],
             (unsigned)m->rssi_bins[6], (unsigned)m->rssi_bins[7],
             (unsigned)m->pdu_bins[0], (unsigned)m->pdu_bins[1], (unsigned)m->pdu_bins[2],
             (unsigned)m->pdu_bins[3], (unsigned)m->pdu_bins[4]);
}

int rf_model_save_nvs(const rf_model_t *m)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READWRITE, &h);
    if (e != ESP_OK) return (int)e;
    e = nvs_set_blob(h, "rf_model", m, sizeof(*m));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (int)e;
}

int rf_model_load_nvs(rf_model_t *m)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("splinter", NVS_READONLY, &h);
    if (e != ESP_OK) return (int)e;
    size_t len = sizeof(*m);
    e = nvs_get_blob(h, "rf_model", m, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(*m) ||
        m->magic != RF_MODEL_MAGIC || m->version != RF_MODEL_VERSION) return -1;
    return 0;
}
