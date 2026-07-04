#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RADAR_VIEW_IDLE_MS 15000u   // no input this long -> back to radar
#define RADAR_BL_IDLE_MS   30000u   // clear + idle this long -> backlight off

typedef enum { RADAR_VIEW_RADAR = 0, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS, RADAR_VIEW_LIBRARY, RADAR_VIEW_COUNT } radar_view_t;

typedef struct {
    radar_view_t view;
    uint32_t last_input_ms;
    uint32_t last_wake_ms;
    bool     backlight_on;
    uint8_t  last_threat_count;
} radar_ui_t;

void radar_ui_reset(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count);
// A user input (button press or screen touch): advance view, wake, reset timers.
void radar_ui_on_input(radar_ui_t *ui, uint32_t now_ms);
// Periodic: idle-return to radar; backlight off when clear+idle; wake+radar on a new follower.
void radar_ui_on_tick(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count);
