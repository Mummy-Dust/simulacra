#include "radar_ui.h"

void radar_ui_reset(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count)
{
    ui->view = RADAR_VIEW_HOME; ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms;
    ui->backlight_on = true; ui->last_threat_count = threat_count;
    ui->sel_preset = 0; ui->send_flash_ms = 0;
}
void radar_ui_select_view(radar_ui_t *ui, radar_view_t v, uint32_t now_ms)
{
    if (v < RADAR_VIEW_COUNT) ui->view = v;
    ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true;
}
void radar_ctrl_select_next(radar_ui_t *ui)
{ ui->sel_preset = (uint8_t)((ui->sel_preset + 1) % RADAR_CTRL_PRESET_COUNT); }
void radar_ctrl_mark_sent(radar_ui_t *ui, uint32_t now_ms) { ui->send_flash_ms = now_ms; }
void radar_ui_note_input(radar_ui_t *ui, uint32_t now_ms)
{ ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true; }
void radar_ui_on_input(radar_ui_t *ui, uint32_t now_ms)   // back-target: return HOME
{
    ui->view = RADAR_VIEW_HOME;
    ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true;
}
void radar_ui_on_tick(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count)
{
    // wake-on-follower: jump to Hunters (RADAR) only if the user is idle
    if (threat_count > ui->last_threat_count) {
        ui->backlight_on = true; ui->last_wake_ms = now_ms;
        if ((uint32_t)(now_ms - ui->last_input_ms) >= RADAR_VIEW_IDLE_MS)
            ui->view = RADAR_VIEW_RADAR;
    }
    ui->last_threat_count = threat_count;
    // idle-return to HOME (only when threats are cleared)
    if (threat_count == 0 && ui->view != RADAR_VIEW_HOME &&
        (uint32_t)(now_ms - ui->last_input_ms) >= RADAR_VIEW_IDLE_MS) ui->view = RADAR_VIEW_HOME;
    if (threat_count == 0 && (uint32_t)(now_ms - ui->last_wake_ms) >= RADAR_BL_IDLE_MS)
        ui->backlight_on = false;
    else if (threat_count > 0) ui->backlight_on = true;
}
