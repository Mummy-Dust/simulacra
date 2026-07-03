#include "radar_geom.h"
#include <math.h>

uint16_t radar_rssi_to_radius(int8_t rssi, uint16_t r_min, uint16_t r_max)
{
    uint16_t band;                                   // 0 near, 1 mid, 2 far
    if      (rssi >= -45) band = 0;
    else if (rssi >= -70) band = 1;
    else                  band = 2;
    return (uint16_t)(r_min + (uint32_t)(r_max - r_min) * band / 2u);
}

uint16_t radar_hash_to_angle(uint32_t hash) { return (uint16_t)(hash % 360u); }

void radar_polar_to_xy(int cx, int cy, uint16_t r, uint16_t angle_deg, int *x, int *y)
{
    double a = (double)angle_deg * (M_PI / 180.0);
    *x = cx + (int)lround((double)r * cos(a));
    *y = cy - (int)lround((double)r * sin(a));
}
