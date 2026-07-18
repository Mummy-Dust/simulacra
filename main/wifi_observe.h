#pragma once
#include <stdbool.h>

// Firmware glue: enable promiscuous RX on the STA interface and feed randomized probe-request
// source MACs into wifi_density (raw MAC hashed-and-dropped there). Our own frames are never
// received (esp32 has no self-RX); fleetmate decoys are excluded via fleet_mac_excluded.

#ifndef WIFI_OBS_FALLBACK
#define WIFI_OBS_FALLBACK 6   // conservative fixed agent target when promiscuous can't start (< PROBE_PHONES)
#endif

// Enable promiscuous probe observe. Returns true iff promiscuous enabled (else caller uses the fallback).
bool wifi_obs_start(void);
