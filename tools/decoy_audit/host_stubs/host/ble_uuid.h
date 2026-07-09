#pragma once
#include <stdint.h>
typedef struct { uint8_t type; uint16_t value; } ble_uuid16_t;
#define BLE_UUID16_INIT(v) { 0x01, (v) }
