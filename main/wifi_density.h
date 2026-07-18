#pragma once
#include <stdint.h>

// Wi-Fi ambient phone-density estimator (PURE; host-testable; no radio). A rolling table of HASHED
// probe-request source MACs (raw MAC never stored -- Law 1) with a TTL; the live distinct count is a
// recently-active real-phone proxy that drives the probe-agent population target (Law 4).

#ifndef WIFI_OBS_TTL_MS
#define WIFI_OBS_TTL_MS 180000u   // forget a MAC not re-heard within ~3 min
#endif
#ifndef WIFI_OBS_FLOOR
#define WIFI_OBS_FLOOR 2          // min fake phones even in a near-empty room (a couple read as plausible)
#endif
#ifndef WIFI_OBS_CAP
#define WIFI_OBS_CAP 16           // = PROBE_AGENTS_MAX; agent ceiling
#endif

void wifi_obs_reset(uint32_t salt);
// Hash `mac` (salted FNV-1a; raw MAC consumed here, never stored) and add/refresh it in the rolling
// table (evict oldest when full). Call only for real (non-fleet, randomized) probe sources.
void wifi_obs_note(const uint8_t mac[6], uint32_t now_ms);
// Distinct non-expired hashed MACs = recently-active real-phone proxy.
int  wifi_obs_density(uint32_t now_ms);
// Density mapped + EWMA-smoothed to a probe-agent target in [WIFI_OBS_FLOOR, WIFI_OBS_CAP].
int  wifi_obs_target(uint32_t now_ms);
