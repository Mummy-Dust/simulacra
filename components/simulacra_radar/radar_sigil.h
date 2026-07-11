#pragma once
#include "radar_gfx.h"
#include "radar_theme.h"

// Draw sigil `id` centered at (cx,cy), fitting within radius r, in color c.
void radar_sigil_draw(radar_gfx_t *g, sigil_id_t id, int cx, int cy, int r, uint16_t c);
