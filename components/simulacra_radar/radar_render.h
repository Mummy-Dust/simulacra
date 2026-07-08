#pragma once
#include "radar_ui.h"
#include "radar_wire.h"
typedef void (*radar_flush_fn)(int y0, int h, const uint16_t *buf, void *ctx);
typedef struct {                 // Vigil librarian snapshot for the LIBRARY page
    bool     sd_ok;
    uint32_t card_mb;            // capacity, 0 if unknown/absent
    uint16_t lib_count, lib_cap;
    uint32_t offer_age_s;        // UINT32_MAX = never
    uint32_t sync_age_s;         // UINT32_MAX = never
    uint32_t save_age_s;         // UINT32_MAX = never
    uint32_t save_bytes;         // size of last sealed blob
} radar_lib_info_t;
typedef struct { uint8_t sel_preset; bool send_flash; } radar_ctrl_info_t;   // CONTROL page state
// Banded full-frame render of `view` from `st` (sweep_deg animates the radar). `band` is a
// scratch buffer of w*band_h uint16; flush() pushes each band to the panel.
// `lib` is the librarian snapshot for RADAR_VIEW_LIBRARY; NULL on non-librarian displays.
// `ctrl` is the CONTROL-page state for RADAR_VIEW_CONTROL; NULL on non-Vigil / non-control.
void radar_render_view(radar_view_t view, const radar_wire_status_t *st,
                       const radar_lib_info_t *lib, const radar_ctrl_info_t *ctrl, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
