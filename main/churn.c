#include <string.h>
#include <stdbool.h>
#include "churn.h"
#include "ble_devices.h"
#include "trace.h"
#include "esp_random.h"

// Presenter state: which device index occupies each hardware instance, and the address last
// applied there (so a rotation — same device, new address — triggers a single re-apply).
static int      s_occ_idx[CHURN_HW_INSTANCES];
static uint8_t  s_occ_addr[CHURN_HW_INSTANCES][6];
static uint32_t s_phase;
static uint32_t s_last_slice_ms;
static churn_apply_fn s_apply;
static bool     s_paused;                            // webui: pause the churn rotation

void    churn_set_apply(churn_apply_fn fn) { s_apply = fn; }
void    churn_set_paused(bool paused) { s_paused = paused; }
bool    churn_paused(void) { return s_paused; }

void churn_init(uint32_t now_ms)
{
    s_phase = 0; s_last_slice_ms = now_ms;
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) { s_occ_idx[i] = -1; memset(s_occ_addr[i], 0, 6); }
}

void churn_tick(uint32_t now_ms)
{
    if (s_paused) return;                          // hold the current on-air set
    ble_devices_tick(now_ms);                      // advance death/rebirth + rotation
    if (now_ms - s_last_slice_ms < CHURN_SLICE_MS) return;
    s_last_slice_ms = now_ms; s_phase++;

    int pop = ble_devices_count();
    if (pop <= 0) return;
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) {
        int idx;
        if (pop <= CHURN_HW_INSTANCES) {
            if (i >= pop) continue;                // fewer devices than radios
            idx = i;
        } else {
            idx = (int)((s_phase * CHURN_HW_INSTANCES + i) % pop);
        }
        const ble_device_t *d = ble_devices_at(idx);
        if (!d) continue;
        if (s_occ_idx[i] != idx || memcmp(s_occ_addr[i], d->id.addr, 6) != 0) {
            s_occ_idx[i] = idx;
            memcpy(s_occ_addr[i], d->id.addr, 6);
            if (s_apply) s_apply((uint8_t)i, &d->id);   // (re)apply this device on instance i
        }
    }
}

size_t churn_active_count(void) { return (size_t)ble_devices_count(); }

const identity_t *churn_active_at(size_t slot)
{
    const ble_device_t *d = ble_devices_at((int)slot);
    return d ? &d->id : 0;
}

// Milestone A: lifetime/rotation owned by ble_devices; these tuning setters are retained
// (inert) below for API compatibility with settings.c/webui.c/coexist.c.
void    churn_set_active_target(uint8_t n) { (void)n; }   // population size owned by ble_devices_init (Milestone A)
uint8_t churn_active_target(void) { return (uint8_t)ble_devices_count(); }
void    churn_set_accel(float mult) { (void)mult; }
void    churn_set_dwell_ms(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }
void    churn_set_cooldown_ms(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }
void    churn_get_dwell_ms(uint32_t *lo, uint32_t *hi) { if (lo) *lo = 0; if (hi) *hi = 0; }
void    churn_get_cooldown_ms(uint32_t *lo, uint32_t *hi) { if (lo) *lo = 0; if (hi) *hi = 0; }
