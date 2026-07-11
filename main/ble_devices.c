#include "ble_devices.h"
#include "roster.h"
#include "esp_random.h"

// Role split (user-chosen): ~70% transient / ~30% resident.
#define ROLE_RESIDENT_PCT   30
// Address-subtype blend, matching roster.c's calibrated fleet mix (~52/36/12 static/RPA/NRPA).
#define ATYPE_STATIC_W  52
#define ATYPE_RPA_W     36
#define ATYPE_NRPA_W    12
// Lifetime bands.
#define TRANSIENT_MIN_MS   120000u    // 2 min
#define TRANSIENT_MAX_MS   720000u    // 12 min
#define RESIDENT_MIN_MS   1800000u    // 30 min
#define RESIDENT_MAX_MS   5400000u    // 90 min
// Rotation cadence per subtype (independent phase + wide jitter). STATIC never rotates.
#define RPA_ROT_MIN_MS     600000u    // 10 min
#define RPA_ROT_MAX_MS    1200000u    // 20 min
#define NRPA_ROT_MIN_MS     60000u    // 1 min
#define NRPA_ROT_MAX_MS    600000u    // 10 min

static ble_device_t s_dev[BLE_DEVICES_MAX];
static int          s_n;

static uint32_t rnd_range(uint32_t lo, uint32_t hi) { return lo + (esp_random() % (hi - lo + 1u)); }

static uint8_t top2_for(ble_atype_t a)
{
    switch (a) { case BLE_ATYPE_STATIC: return 0xC0; case BLE_ATYPE_RPA: return 0x40;
                 default: return 0x00; }   // NRPA
}

static ble_atype_t pick_atype(void)
{
    uint32_t r = esp_random() % (ATYPE_STATIC_W + ATYPE_RPA_W + ATYPE_NRPA_W);
    if (r < ATYPE_STATIC_W) return BLE_ATYPE_STATIC;
    if (r < ATYPE_STATIC_W + ATYPE_RPA_W) return BLE_ATYPE_RPA;
    return BLE_ATYPE_NRPA;
}

static uint32_t rotate_base(ble_atype_t a)
{
    switch (a) {
        case BLE_ATYPE_RPA:  return rnd_range(RPA_ROT_MIN_MS,  RPA_ROT_MAX_MS);
        case BLE_ATYPE_NRPA: return rnd_range(NRPA_ROT_MIN_MS, NRPA_ROT_MAX_MS);
        default:             return 0;   // STATIC: unused
    }
}

// Draw a frozen behaviour (payload/itvl/company/tx/archetype) from the roster library and
// stamp a fresh address of the chosen subtype. The roster entry's own address is discarded.
static void dev_spawn(ble_device_t *d, uint32_t now_ms)
{
    identity_t *src = roster_at(esp_random() % CHURN_ROSTER_SIZE);
    d->id = *src;                                   // copy behaviour (and its addr, overwritten next)
    d->atype = pick_atype();
    make_random_addr(d->id.addr, top2_for(d->atype));
    d->role = (esp_random() % 100u < ROLE_RESIDENT_PCT) ? BLE_ROLE_RESIDENT : BLE_ROLE_TRANSIENT;
    d->born_ms = now_ms;
    d->life_ms = (d->role == BLE_ROLE_RESIDENT) ? rnd_range(RESIDENT_MIN_MS, RESIDENT_MAX_MS)
                                                : rnd_range(TRANSIENT_MIN_MS, TRANSIENT_MAX_MS);
    d->alive = true;
    // Independent rotation phase: first rotation is a full jittered interval out from birth.
    d->next_rotate_ms = (d->atype == BLE_ATYPE_STATIC) ? 0 : now_ms + rotate_base(d->atype);
}

void ble_devices_init(int n, uint32_t now_ms)
{
    if (n > BLE_DEVICES_MAX) n = BLE_DEVICES_MAX;
    if (n < 1) n = 1;
    s_n = n;
    for (int i = 0; i < s_n; i++) dev_spawn(&s_dev[i], now_ms);
}

int ble_devices_count(void) { return s_n; }
const ble_device_t *ble_devices_at(int i) { return (i >= 0 && i < s_n) ? &s_dev[i] : 0; }

void ble_devices_tick(uint32_t now_ms)
{
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (d->alive && (now_ms - d->born_ms) >= d->life_ms) {
            dev_spawn(d, now_ms);          // dies; reborn fresh (new subtype/role/behaviour/addr/phase)
        }
    }
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (!d->alive) continue;
        if (d->atype == BLE_ATYPE_STATIC) continue;
        if ((int32_t)(now_ms - d->next_rotate_ms) >= 0) {
            make_random_addr(d->id.addr, top2_for(d->atype));
            d->next_rotate_ms = now_ms + rotate_base(d->atype);
        }
    }
}
