#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RF_MODEL_MAGIC    0x52464D31u   // "RFM1"
#define RF_MODEL_VERSION  1
#define RF_VENDOR_SLOTS   24
#define RF_ITVL_BINS      7    // <50,50-100,100-200,200-500,500-1000,1000-2000,>2000 ms
#define RF_RSSI_BINS      8    // -100..-20 dBm in 10 dBm steps
#define RF_PDU_BINS       5    // ADV_IND,DIR_IND,SCAN_IND,NONCONN_IND,SCAN_RSP (NimBLE evtype 0..4)
#define RF_VENDOR_UNKNOWN 0xFFFF  // no mfg-data / unknown company id

typedef struct {
    uint16_t company_id;
    uint32_t count;
    uint32_t itvl_bins[RF_ITVL_BINS];
} rf_vendor_t;

typedef struct {
    uint32_t    magic;
    uint16_t    version;
    uint32_t    sweeps;
    uint32_t    total_obs;
    rf_vendor_t vendors[RF_VENDOR_SLOTS];
    uint32_t    other_count;
    uint32_t    other_itvl_bins[RF_ITVL_BINS];
    uint32_t    rssi_bins[RF_RSSI_BINS];
    uint32_t    pdu_bins[RF_PDU_BINS];
    float       pop_ewma;          // EWMA of distinct devices per sweep
    float       arrival_per_min;   // EWMA of new distinct devices per minute
} rf_model_t;

#define RF_DECAY_DEN 4   // rolling-window decay: retain (DEN-1)/DEN of each histogram per closed sweep

void   rf_model_reset(rf_model_t *m);
// Fade old observations into a rolling window so the model tracks the RECENT environment and no
// single loud vendor accumulates unbounded, permanent weight. Call once per closed sweep.
void   rf_model_decay(rf_model_t *m);
// Fold one observation's static features. interval_ms < 0 => no interval sample (first sighting).
void   rf_model_observe(rf_model_t *m, uint16_t company_id, int8_t rssi,
                        uint8_t pdu_type, int32_t interval_ms);
// Fold a completed sweep's distinct-device aggregates (EWMA).
void   rf_model_end_sweep(rf_model_t *m, uint32_t distinct_devices, uint32_t window_ms,
                          uint32_t arrivals);

size_t rf_itvl_bin(int32_t ms);
size_t rf_rssi_bin(int8_t rssi);
size_t rf_pdu_bin(uint8_t pdu_type);
int    rf_vendor_index(const rf_model_t *m, uint16_t company_id);  // occupied slot or -1

void   rf_model_dump(const rf_model_t *m);

// NVS persistence: namespace "splinter", key "rf_model". Return 0 on success.
int    rf_model_save_nvs(const rf_model_t *m);
int    rf_model_load_nvs(rf_model_t *m);   // 0 and fills m if a valid current-version blob exists
