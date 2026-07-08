#pragma once
#include "webui.h"        // webui_status_t
#include "radar_wire.h"   // radar_wire_status_t

// Pack a live decoy snapshot into the wire view-model (pure; unit-tested).
void espnow_status_from_webui(radar_wire_status_t *out, const webui_status_t *in);

// Open a sealed frame with the current fleet key, then the previous key (rotation
// grace). Returns 0 (outputs filled) or -1. With provisioning off, current = the
// compile-time key and previous = NULL, so behavior matches the legacy single-key open.
int espnow_open_any(const uint8_t *frame, size_t flen, uint8_t *out_type, uint8_t *payload,
                    size_t *payload_len, uint8_t out_salt[4], uint64_t *out_counter);

// Init ESP-NOW responder + start its task (defined in Task 5). Gated by SIMULACRA_ESPNOW.
void esp_now_link_start(void);
