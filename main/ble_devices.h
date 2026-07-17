#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "identity.h"

// Max concurrent persistent devices per board. The runtime population (set at init) is
// clamped to this. Perceived density comes from turnover, not from raising this ceiling.
#define BLE_DEVICES_MAX 32

typedef enum { BLE_ATYPE_STATIC, BLE_ATYPE_RPA, BLE_ATYPE_NRPA } ble_atype_t;
// PERSISTENT = long-lived static "infrastructure" (beacons/fixtures): one address held for hours,
// matching the real ambient >2h presence tail that a purely-churning fleet lacks. Always static
// (only a non-rotating address can actually persist on air).
typedef enum { BLE_ROLE_TRANSIENT, BLE_ROLE_RESIDENT, BLE_ROLE_PERSISTENT } ble_role_t;

typedef struct {
    identity_t  id;             // advertising identity: addr + frozen behaviour (payload/itvl/tx/company/arch)
    ble_atype_t atype;          // fixed for life; selects rotation policy (STATIC never rotates)
    ble_role_t  role;           // fixed for life; selects the lifetime band
    uint32_t    born_ms;        // set at spawn; == now on a fresh birth/rebirth
    uint32_t    life_ms;        // bounded lifetime; on expiry the device dies and is reborn fresh
    uint32_t    next_rotate_ms; // next address rotation (ignored for STATIC)
    bool        alive;
    int8_t   persona_idx;       // >=0: bound to phantom[persona_idx]; -1: unbound BLE-only crowd
    uint32_t persona_gen;       // last phantom generation this bound member synced to
} ble_device_t;

// Spawn `n` persistent devices (clamped to BLE_DEVICES_MAX). Behaviour is drawn from the
// roster library, so roster_init() MUST have been called first.
void  ble_devices_init(int n, uint32_t now_ms);
// One scheduler tick: retire+reincarnate expired devices, then rotate the address of any
// rotating-subtype device whose next_rotate_ms has passed. Behaviour is preserved across a
// rotation; only addr changes.
void  ble_devices_tick(uint32_t now_ms);
int   ble_devices_count(void);
const ble_device_t *ble_devices_at(int i);
// Shade-form breakdown of the live population by address subtype: restless=RPA (rotating),
// wandering=NRPA (rotating, no resolvable identity), bound=static (never rotates).
void  ble_devices_form_counts(uint8_t *restless, uint8_t *wandering, uint8_t *bound);

// Bind BLE slot `slot` to phantom `persona_idx` (see phantom.h): when the phantom's generation
// advances, reincarnate the slot as an RPA device carrying a Law-3-safe phone advertisement
// (flags-only / 16-bit service-UUID list, no manufacturer data), the phantom's shared born/life,
// and a fresh unique address. `apple` selects the iPhone floor (flags-only, no Continuity).
// Returns 1 if reincarnated. Bound slots do NOT expire via ble_devices_tick; the phantom owns them.
int ble_device_sync(int slot, int persona_idx, bool apple,
                    uint32_t born_ms, uint32_t life_ms, uint32_t generation);
