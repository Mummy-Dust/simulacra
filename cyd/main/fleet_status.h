#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef _MSC_VER
#define __attribute__(x)
#endif
#include "radar_wire.h"

#define FLEET_STATUS_MAX       4
#define FLEET_STATUS_STALE_MS  12000u   // no status this long -> node reads "silent"

typedef struct { uint8_t id; radar_wire_status_t st; uint32_t last_ms; bool used; } fleet_node_t;
typedef struct { fleet_node_t nodes[FLEET_STATUS_MAX]; } fleet_status_t;

void   fleet_status_reset(fleet_status_t *f);
void   fleet_status_upsert(fleet_status_t *f, uint8_t node_id, const radar_wire_status_t *st, uint32_t now_ms);
int    fleet_status_count(const fleet_status_t *f);                       // used slots
bool   fleet_status_at(const fleet_status_t *f, int i, uint8_t *id,
                       const radar_wire_status_t **st, bool *alive, uint32_t now_ms);
// Fold every ALIVE node into one fleet-wide status for the sub-views: counts sum (saturating),
// epoch/uptime/pop take the max, flags OR together, and threats union by hash (closest RSSI +
// strongest recurrence/known-class kept), capped at RADAR_MAX_THREATS. `out` is fully written.
void   fleet_status_aggregate(const fleet_status_t *f, uint32_t now_ms, radar_wire_status_t *out);
