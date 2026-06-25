#pragma once
#include <stdint.h>

typedef enum { ID_IDLE, ID_ACTIVE, ID_COOLDOWN } id_state_t;

typedef struct {
    uint8_t    addr[6];          // stable random-static MAC (top 2 bits set)
    uint16_t   company_id;       // vendor company id (debug/inspection)
    uint8_t    payload[31];      // frozen, serialized AD bytes
    uint8_t    payload_len;
    uint16_t   adv_itvl_ms;      // this identity's on-air interval
    id_state_t state;
    uint32_t   active_until_ms;  // ACTIVE: dwell deadline (absolute ms)
    uint32_t   eligible_at_ms;   // COOLDOWN: earliest re-promotion time (absolute ms)
} identity_t;
