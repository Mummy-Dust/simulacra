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
