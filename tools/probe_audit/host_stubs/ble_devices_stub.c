// Link-only stub for the probe_audit host harness.
//
// main/phantom.c (Task 5, commit 7ce1bed) added phantom_sync_ble(), which calls into
// main/ble_devices.c (ble_devices_count/ble_device_sync). The probe_audit harness only
// ever exercises the Wi-Fi-side pure core (probe_frame.c/probe_agents.c/uniq_id.c/phantom.c)
// -- none of its tests call phantom_sync_ble or otherwise touch BLE behaviour (the BLE-side
// binding is exercised by tools/decoy_audit/synth_dump.c, which links the real ble_devices.c).
// Pulling the real ble_devices.c in here would drag in its roster.c/generate.c/templates.c
// dependency chain for zero test value, so this stub only satisfies the linker.
#include "ble_devices.h"

int ble_devices_count(void) { return 0; }

int ble_device_sync(int slot, int persona_idx, bool apple,
                     uint32_t born_ms, uint32_t life_ms, uint32_t generation)
{
    (void)slot; (void)persona_idx; (void)apple;
    (void)born_ms; (void)life_ms; (void)generation;
    return 0;
}
