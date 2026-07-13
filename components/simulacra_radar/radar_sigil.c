#include "radar_sigil.h"

static void star5(radar_gfx_t *g, int cx, int cy, int r, uint16_t c)
{
    // 5-point star via 5 outer points connected pentagram-style (indices step by 2).
    int px[5], py[5];
    // precomputed unit pentagon points (x1000), starting at top, clockwise
    static const int ux[5] = {0, 951, 588, -588, -951};
    static const int uy[5] = {-1000, -309, 809, 809, -309};
    for (int i = 0; i < 5; i++) { px[i] = cx + ux[i]*r/1000; py[i] = cy + uy[i]*r/1000; }
    for (int i = 0; i < 5; i++) radar_gfx_line(g, px[i], py[i], px[(i+2)%5], py[(i+2)%5], c);
}

void radar_sigil_draw(radar_gfx_t *g, sigil_id_t id, int cx, int cy, int r, uint16_t c)
{
    switch (id) {
    case SIGIL_CIRCLE:                       // ring + inscribed pentagram (the coven)
        radar_gfx_circle(g, cx, cy, r, c);
        star5(g, cx, cy, r - 2, c);
        break;
    case SIGIL_HUNTER: {                      // watching eye
        radar_gfx_line(g, cx-r, cy, cx, cy-r*3/5, c);
        radar_gfx_line(g, cx, cy-r*3/5, cx+r, cy, c);
        radar_gfx_line(g, cx+r, cy, cx, cy+r*3/5, c);
        radar_gfx_line(g, cx, cy+r*3/5, cx-r, cy, c);
        radar_gfx_circle(g, cx, cy, r/3, c);
        radar_gfx_fill_rect(g, cx-1, cy-1, 2, 2, c);
        break; }
    case SIGIL_LIVING: {                      // heartbeat
        radar_gfx_hline(g, cx-r, cx-r/3, cy, c);
        radar_gfx_line(g, cx-r/3, cy, cx-r/6, cy-r, c);
        radar_gfx_line(g, cx-r/6, cy-r, cx+r/8, cy+r, c);
        radar_gfx_line(g, cx+r/8, cy+r, cx+r/3, cy, c);
        radar_gfx_hline(g, cx+r/3, cx+r, cy, c);
        break; }
    case SIGIL_RITE:                          // star
        star5(g, cx, cy, r, c);
        radar_gfx_circle(g, cx, cy, r, c);
        break;
    case SIGIL_WARD:                          // key-eye (circle + shaft)
        radar_gfx_circle(g, cx-r/3, cy-r/3, r/2, c);
        radar_gfx_line(g, cx, cy, cx+r, cy+r, c);
        radar_gfx_line(g, cx+r*2/3, cy+r, cx+r, cy+r, c);
        break;
    case SIGIL_GRIMOIRE:                      // tome
        radar_gfx_fill_rect(g, cx-r, cy-r, 2, 2*r, c);        // spine
        radar_gfx_line(g, cx-r, cy-r, cx+r, cy-r, c);
        radar_gfx_line(g, cx-r, cy+r, cx+r, cy+r, c);
        radar_gfx_vline(g, cx+r, cy-r, cy+r, c);
        radar_gfx_hline(g, cx-r/2, cx+r/2, cy-r/3, c);
        radar_gfx_hline(g, cx-r/2, cx+r/2, cy, c);
        break;
    default: break;
    }
}
