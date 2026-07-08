#include "radar_ui.h"

void radar_ui_reset(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count)
{
    ui->view = RADAR_VIEW_RADAR; ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms;
    ui->backlight_on = true; ui->last_threat_count = threat_count;
    ui->sel_preset = 2;         // default highlight = NORMAL
    ui->send_flash_ms = 0;
}
void radar_ctrl_select_next(radar_ui_t *ui)
{ ui->sel_preset = (uint8_t)((ui->sel_preset + 1) % RADAR_CTRL_PRESET_COUNT); }
void radar_ctrl_mark_sent(radar_ui_t *ui, uint32_t now_ms) { ui->send_flash_ms = now_ms; }
void radar_ui_note_input(radar_ui_t *ui, uint32_t now_ms)
{ ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true; }
void radar_ui_on_input(radar_ui_t *ui, uint32_t now_ms)
{
    ui->view = (radar_view_t)((ui->view + 1) % RADAR_VIEW_COUNT);
    ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true;
}
void radar_ui_on_tick(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count)
{
    if (threat_count > ui->last_threat_count) {
        ui->backlight_on = true; ui->last_wake_ms = now_ms;
        // Surface a new follower by jumping to the radar page ONLY if the user isn't actively
        // navigating menus; yanking them off a page they just opened is the random snap-back bug.
        // Once they go idle the shared idle-return below brings them to radar anyway.
        if ((uint32_t)(now_ms - ui->last_input_ms) >= RADAR_VIEW_IDLE_MS)
            ui->view = RADAR_VIEW_RADAR;
    }
    ui->last_threat_count = threat_count;
    if (ui->view != RADAR_VIEW_RADAR &&
        (uint32_t)(now_ms - ui->last_input_ms) >= RADAR_VIEW_IDLE_MS) ui->view = RADAR_VIEW_RADAR;
    if (threat_count == 0 && (uint32_t)(now_ms - ui->last_wake_ms) >= RADAR_BL_IDLE_MS)
        ui->backlight_on = false;
    else if (threat_count > 0) ui->backlight_on = true;
}
