#pragma once
#include <stdint.h>

// Map a follower's best RSSI (dBm, negative) to a radar radius in px. Stronger (less
// negative) -> smaller radius (nearer center). Banded: >=-45 near, -45..-70 mid, <-70 far.
// Result within [r_min, r_max].
uint16_t radar_rssi_to_radius(int8_t rssi, uint16_t r_min, uint16_t r_max);

// Stable pseudo-bearing from a hash: [0,359] deg. Same hash -> same angle (synthetic; a
// single antenna cannot triangulate — labeled as such on-screen).
uint16_t radar_hash_to_angle(uint32_t hash);

// Polar (center cx,cy; radius r; angle deg, 0=east, CCW) -> screen x,y (y grows down).
void radar_polar_to_xy(int cx, int cy, uint16_t r, uint16_t angle_deg, int *x, int *y);
