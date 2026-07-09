#include <string.h>
#include "host/ble_hs.h"
static int put(uint8_t *buf, uint8_t *n, uint8_t max, uint8_t type,
               const uint8_t *val, uint8_t vlen)
{
    if ((int)*n + 2 + vlen > max) return 1;
    buf[(*n)++] = (uint8_t)(1 + vlen);
    buf[(*n)++] = type;
    if (vlen) { memcpy(buf + *n, val, vlen); *n += vlen; }
    return 0;
}
int ble_hs_adv_set_fields(const struct ble_hs_adv_fields *f, uint8_t *buf,
                          uint8_t *out_len, uint8_t max_sz)
{
    uint8_t n = 0;
    if (f->flags && put(buf, &n, max_sz, 0x01, &f->flags, 1)) return 1;
    if (f->num_uuids16) {
        uint8_t u[2] = { (uint8_t)(f->uuids16->value & 0xff),
                         (uint8_t)(f->uuids16->value >> 8) };
        if (put(buf, &n, max_sz, f->uuids16_is_complete ? 0x03 : 0x02, u, 2)) return 1;
    }
    if (f->name && f->name_len &&
        put(buf, &n, max_sz, f->name_is_complete ? 0x09 : 0x08, f->name, f->name_len)) return 1;
    if (f->svc_data_uuid16 && f->svc_data_uuid16_len &&
        put(buf, &n, max_sz, 0x16, f->svc_data_uuid16, f->svc_data_uuid16_len)) return 1;
    if (f->mfg_data && f->mfg_data_len &&
        put(buf, &n, max_sz, 0xFF, f->mfg_data, f->mfg_data_len)) return 1;
    *out_len = n;
    return 0;
}
