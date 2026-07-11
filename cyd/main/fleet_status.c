#include "fleet_status.h"
#include <string.h>

void fleet_status_reset(fleet_status_t *f){ memset(f, 0, sizeof(*f)); }

void fleet_status_upsert(fleet_status_t *f, uint8_t node_id, const radar_wire_status_t *st, uint32_t now_ms)
{
    int free_slot = -1;
    for (int i = 0; i < FLEET_STATUS_MAX; i++) {
        if (f->nodes[i].used && f->nodes[i].id == node_id) {
            f->nodes[i].st = *st; f->nodes[i].last_ms = now_ms; return;
        }
        if (!f->nodes[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        f->nodes[free_slot].used = true; f->nodes[free_slot].id = node_id;
        f->nodes[free_slot].st = *st; f->nodes[free_slot].last_ms = now_ms;
    }   // table full: drop (v1 fleet <= FLEET_STATUS_MAX)
}

int fleet_status_count(const fleet_status_t *f)
{ int n = 0; for (int i = 0; i < FLEET_STATUS_MAX; i++) if (f->nodes[i].used) n++; return n; }

bool fleet_status_at(const fleet_status_t *f, int i, uint8_t *id,
                     const radar_wire_status_t **st, bool *alive, uint32_t now_ms)
{
    int seen = 0;
    for (int k = 0; k < FLEET_STATUS_MAX; k++) {
        if (!f->nodes[k].used) continue;
        if (seen++ != i) continue;
        if (id) *id = f->nodes[k].id;
        if (st) *st = &f->nodes[k].st;
        if (alive) *alive = (uint32_t)(now_ms - f->nodes[k].last_ms) < FLEET_STATUS_STALE_MS;
        return true;
    }
    return false;
}
