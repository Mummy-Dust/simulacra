#pragma once
#include "radar_ui.h"
#include "radar_wire.h"
typedef void (*radar_flush_fn)(int y0, int h, const uint16_t *buf, void *ctx);
// Banded full-frame render of `view` from `st` (sweep_deg animates the radar). `band` is a
// scratch buffer of w*band_h uint16; flush() pushes each band to the panel.
void radar_render_view(radar_view_t view, const radar_wire_status_t *st, uint16_t sweep_deg,
                       uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx);
