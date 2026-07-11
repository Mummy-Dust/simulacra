#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RADAR_VIEW_IDLE_MS 15000u   // no input this long -> back to home (while no threat)
#define RADAR_BL_IDLE_MS   30000u   // clear + idle this long -> backlight off

typedef enum { RADAR_VIEW_HOME = 0, RADAR_VIEW_RADAR, RADAR_VIEW_DETAIL, RADAR_VIEW_STATS,
               RADAR_VIEW_LIBRARY, RADAR_VIEW_CONTROL, RADAR_VIEW_INFO, RADAR_VIEW_COUNT } radar_view_t;

#define RADAR_CTRL_PRESET_COUNT 5
#define RADAR_CTRL_FLASH_MS     1200u

typedef struct {
    radar_view_t view;
    uint32_t last_input_ms;
    uint32_t last_wake_ms;
    bool     backlight_on;
    uint8_t  last_threat_count;
    uint8_t  sel_preset;        // CONTROL page: highlighted preset 0..RADAR_CTRL_PRESET_COUNT-1
    uint32_t send_flash_ms;     // CONTROL page: timestamp of last SEND (drives the SENT flash)
} radar_ui_t;

void radar_ui_reset(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count);
// Jump directly to a chosen view (home-grid tap). Wakes + resets timers.
void radar_ui_select_view(radar_ui_t *ui, radar_view_t v, uint32_t now_ms);
// A user input off-home (screen touch / back-target): return to home, wake, reset timers.
void radar_ui_on_input(radar_ui_t *ui, uint32_t now_ms);
// Periodic: idle-return to home (while clear); backlight off when clear+idle; wake to Hunters on a new follower.
void radar_ui_on_tick(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count);
// CONTROL page: cycle the highlighted preset.
void radar_ctrl_select_next(radar_ui_t *ui);
// CONTROL page: stamp the SENT flash timer.
void radar_ctrl_mark_sent(radar_ui_t *ui, uint32_t now_ms);
// Refresh wake/idle timers without changing the view (CONTROL page in-page taps).
void radar_ui_note_input(radar_ui_t *ui, uint32_t now_ms);
