#pragma once
#include <stdint.h>
#include "ble_uuid.h"
#define BLE_HS_ADV_MAX_SZ        31
#define BLE_HS_ADV_F_DISC_GEN    0x02
#define BLE_HS_ADV_F_BREDR_UNSUP 0x04
struct ble_hs_adv_fields {
    const uint8_t     *mfg_data;        uint8_t mfg_data_len;
    uint8_t            flags;
    const uint8_t     *name;            uint8_t name_len; unsigned name_is_complete:1;
    const ble_uuid16_t *uuids16;        uint8_t num_uuids16; unsigned uuids16_is_complete:1;
    const uint8_t     *svc_data_uuid16; uint8_t svc_data_uuid16_len;
};
/* Serialize present fields into AD TLVs in NimBLE's canonical relative order:
   flags(0x01), uuids16(0x02/0x03), name(0x08/0x09), svc_data16(0x16), mfg(0xFF).
   Returns 0 on success, 1 if the result would exceed max_sz. */
int ble_hs_adv_set_fields(const struct ble_hs_adv_fields *f, uint8_t *buf,
                          uint8_t *out_len, uint8_t max_sz);
