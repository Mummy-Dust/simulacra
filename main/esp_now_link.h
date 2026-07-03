#pragma once
#include "webui.h"        // webui_status_t
#include "radar_wire.h"   // radar_wire_status_t

// Pack a live decoy snapshot into the wire view-model (pure; unit-tested).
void espnow_status_from_webui(radar_wire_status_t *out, const webui_status_t *in);

// Init ESP-NOW responder + start its task (defined in Task 5). Gated by SIMULACRA_ESPNOW.
void esp_now_link_start(void);
