#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "probe_frame.h"   // PROBE_FRAME_MAX, archetypes, probe_random_mac, probe_build_request
#include "probe_agents.h"  // independent per-phone agents: seq, lifecycle, due-selection

// Start Wi-Fi raw TX + the injection loop (Wi-Fi-only mode; BLE idle).
void   probe_start(void);

// --- scheduler-callable core (M8 coexistence) ---
// Initialize Wi-Fi STA for raw injection. Tolerates an already-created default event loop
// (NimBLE/coexist may run alongside). Returns 0 on success, else the failing esp_err_t.
int    probe_wifi_init(void);
// Seed the persona-sized fake-phone MAC pool. Call once after probe_wifi_init.
void   probe_pool_init(void);
// Inject one burst on `channel`: set the channel, TX a wildcard probe-req from each fake
// phone, occasionally rotate one MAC. Returns the last esp_wifi_80211_tx rc (0 = ok).
int    probe_inject_burst(uint8_t channel);
// Number of fake phones in the pool (persona-sized).
int    probe_phone_count(void);
// Cumulative count of probe requests injected since boot (dashboard telemetry).
uint32_t probe_total_sent(void);
// TX health: false once too many consecutive injections have failed (alive but not transmitting).
bool   probe_tx_healthy(void);
// Channel hop sets for the scheduler: fills *out, returns count. 5g returns 0 on 2.4-only personas.
size_t probe_channels_24(const uint8_t **out);
size_t probe_channels_5g(const uint8_t **out);
// Minimum BLE population so every persona gets a co-present twin (PROBE_PHONES bound +
// PHANTOM_BLE_UNBOUND unbound), clamped to BLE_DEVICES_MAX.
int probe_desired_ble_floor(void);   // min BLE population so every persona gets a co-present twin
int probe_phone_target(void);        // persona/agent count (PROBE_PHONES); for early phantom_init
