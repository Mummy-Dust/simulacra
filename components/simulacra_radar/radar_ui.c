#include "radar_ui.h"

void radar_ui_reset(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count)
{
    ui->view = RADAR_VIEW_RADAR; ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms;
    ui->backlight_on = true; ui->last_threat_count = threat_count;
}
void radar_ui_on_input(radar_ui_t *ui, uint32_t now_ms)
{
    ui->view = (radar_view_t)((ui->view + 1) % RADAR_VIEW_COUNT);
    ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true;
}
void radar_ui_on_tick(radar_ui_t *ui, uint32_t now_ms, uint8_t threat_count)
{
    if (threat_count > ui->last_threat_count) {
        ui->backlight_on = true; ui->view = RADAR_VIEW_RADAR; ui->last_wake_ms = now_ms;
    }
    ui->last_threat_count = threat_count;
    if (ui->view != RADAR_VIEW_RADAR &&
        (uint32_t)(now_ms - ui->last_input_ms) >= RADAR_VIEW_IDLE_MS) ui->view = RADAR_VIEW_RADAR;
    if (threat_count == 0 && (uint32_t)(now_ms - ui->last_wake_ms) >= RADAR_BL_IDLE_MS)
        ui->backlight_on = false;
    else if (threat_count > 0) ui->backlight_on = true;
}
