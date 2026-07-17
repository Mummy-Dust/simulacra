#include "ble_devices.h"
#include "roster.h"
#include "templates.h"
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
// Persistent "infrastructure" band: a slice of STATIC devices hold one address for hours, matching
// the real ambient >2h presence tail. ~28% of the static 52% -> ~15% of the fleet.
#define PERSISTENT_PCT_OF_STATIC  28
#define PERSISTENT_MIN_MS  14400000u  // 4 h  (outlives any normal session -> the address persists)
#define PERSISTENT_MAX_MS  43200000u  // 12 h
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
    // A slice of static devices are PERSISTENT infrastructure (one address held for hours);
    // everyone else churns on the transient/resident bands. Only static can persist on air --
    // an RPA/NRPA device rotates its address regardless of how long the device itself lives.
    if (d->atype == BLE_ATYPE_STATIC && (esp_random() % 100u) < PERSISTENT_PCT_OF_STATIC) {
        d->role    = BLE_ROLE_PERSISTENT;
        d->life_ms = rnd_range(PERSISTENT_MIN_MS, PERSISTENT_MAX_MS);
    } else {
        d->role    = (esp_random() % 100u < ROLE_RESIDENT_PCT) ? BLE_ROLE_RESIDENT : BLE_ROLE_TRANSIENT;
        d->life_ms = (d->role == BLE_ROLE_RESIDENT) ? rnd_range(RESIDENT_MIN_MS, RESIDENT_MAX_MS)
                                                    : rnd_range(TRANSIENT_MIN_MS, TRANSIENT_MAX_MS);
    }
    d->born_ms = now_ms;
    d->alive = true;
    // Independent rotation phase: first rotation is a full jittered interval out from birth.
    d->next_rotate_ms = (d->atype == BLE_ATYPE_STATIC) ? 0 : now_ms + rotate_base(d->atype);
    d->persona_idx = -1;        // unbound by default; phantom_sync_ble claims bound slots
    d->persona_gen = 0;
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

void ble_devices_form_counts(uint8_t *restless, uint8_t *wandering, uint8_t *bound)
{
    uint8_t r=0,w=0,b=0;
    for (int i = 0; i < s_n; i++) {
        if (!s_dev[i].alive) continue;
        switch (s_dev[i].atype) {
            case BLE_ATYPE_RPA:  r++; break;   // restless (rotating)
            case BLE_ATYPE_NRPA: w++; break;   // wandering
            default:             b++; break;   // static -> bound
        }
    }
    if (restless)  *restless  = r;
    if (wandering) *wandering = w;
    if (bound)     *bound     = b;
}

void ble_devices_tick(uint32_t now_ms)
{
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (d->persona_idx >= 0) continue;                 // bound: phantom owns lifecycle
        if (d->alive && (now_ms - d->born_ms) >= d->life_ms) {
            dev_spawn(d, now_ms);          // dies; reborn fresh (new subtype/role/behaviour/addr/phase)
        }
    }
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (!d->alive) continue;
        if (d->persona_idx >= 0) continue;                 // bound RPA rotation handled below
        if (d->atype == BLE_ATYPE_STATIC) continue;
        if ((int32_t)(now_ms - d->next_rotate_ms) >= 0) {
            make_random_addr(d->id.addr, top2_for(d->atype));
            d->next_rotate_ms = now_ms + rotate_base(d->atype);
        }
    }
}

int ble_device_sync(int slot, int persona_idx, bool apple,
                    uint32_t born_ms, uint32_t life_ms, uint32_t generation)
{
    if (slot < 0 || slot >= s_n) return 0;
    ble_device_t *d = &s_dev[slot];
    if (d->persona_idx == persona_idx && d->persona_gen == generation && d->alive) return 0;
    // A phone presents on BLE as a terse phone shape (flags-only / svc-uuid16), never accessory
    // manufacturer data. Build it directly (no roster draw); company id stays 0.
    d->id.company_id    = 0;
    d->id.tx_power      = 0;
    d->id.archetype_idx = 0;
    if (template_build_phone(apple, d->id.payload, &d->id.payload_len, &d->id.adv_itvl_ms) != 0)
        d->id.payload_len = 0;                          // serialization guard (self-test catches)
    d->atype = BLE_ATYPE_RPA;                           // phones present on BLE as RPA
    make_random_addr(d->id.addr, top2_for(BLE_ATYPE_RPA));   // fresh unique RPA address
    d->role   = BLE_ROLE_TRANSIENT;                     // lifetime is the phantom's, not a band
    d->born_ms = born_ms;
    d->life_ms = life_ms;
    d->alive = true;
    d->next_rotate_ms = born_ms + rotate_base(BLE_ATYPE_RPA);
    d->persona_idx = (int8_t)persona_idx;
    d->persona_gen = generation;
    return 1;
}
