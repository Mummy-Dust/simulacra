#pragma once
#include <stdint.h>
#include <stddef.h>

#define PROBE_FRAME_MAX 64   // header(24) + wildcard SSID + rates + ext rates + DS param < 64

// Fill a randomized locally-administered, unicast MAC (Wi-Fi analog of BLE random-static).
void   probe_random_mac(uint8_t out[6]);
// Build a broadcast (wildcard-SSID) probe request for source `mac` on `channel`.
// Writes the 802.11 frame to out (<= PROBE_FRAME_MAX) and its length. Returns 0 on success.
int    probe_build_request(const uint8_t mac[6], uint8_t channel, uint8_t *out, size_t *out_len);

// Start Wi-Fi raw TX + the injection loop (Wi-Fi-only mode; BLE idle). Implemented in Task 2.
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
// Channel hop sets for the scheduler: fills *out, returns count. 5g returns 0 on 2.4-only personas.
size_t probe_channels_24(const uint8_t **out);
size_t probe_channels_5g(const uint8_t **out);
