#pragma once
#include <stdint.h>
typedef struct { uint16_t *buf; int w; int y0; int h; } radar_gfx_t;   // band = rows [y0, y0+h)
void radar_gfx_clear(radar_gfx_t *g, uint16_t c);
void radar_gfx_pixel(radar_gfx_t *g, int x, int y, uint16_t c);
void radar_gfx_hline(radar_gfx_t *g, int x0, int x1, int y, uint16_t c);
void radar_gfx_vline(radar_gfx_t *g, int x, int y0, int y1, uint16_t c);
void radar_gfx_line(radar_gfx_t *g, int x0, int y0, int x1, int y1, uint16_t c);
void radar_gfx_fill_rect(radar_gfx_t *g, int x0, int y0, int w, int h, uint16_t c);
void radar_gfx_circle(radar_gfx_t *g, int cx, int cy, int r, uint16_t c);
void radar_gfx_text(radar_gfx_t *g, int x, int y, const char *s, uint16_t c);
