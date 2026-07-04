#include "radar_render.h"
#include "radar_gfx.h"
#include "radar_geom.h"
#include "sig_class_name.h"
#include <stdio.h>
#define COL_BG 0x0000
#define COL_FG 0xFFFF
#define COL_DIM 0x7BEF
#define COL_RING 0x2965
#define COL_OK 0x07E0
#define COL_WARN 0xF800
#define COL_SWEEP 0x0400
#define RCX 120
#define RCY 120
#define RR 100

static uint16_t threat_color(uint8_t ep){ return ep>=5?COL_WARN:(ep>=2?0xFD20:0xFFE0); }

static void draw_radar(radar_gfx_t *g, const radar_wire_status_t *st, uint16_t sweep){
    radar_gfx_circle(g,RCX,RCY,RR,COL_RING); radar_gfx_circle(g,RCX,RCY,RR*2/3,COL_RING);
    radar_gfx_circle(g,RCX,RCY,RR/3,COL_RING);
    radar_gfx_hline(g,RCX-RR,RCX+RR,RCY,COL_RING); radar_gfx_vline(g,RCX,RCY-RR,RCY+RR,COL_RING);
    int sx,sy; radar_polar_to_xy(RCX,RCY,RR,sweep,&sx,&sy); radar_gfx_line(g,RCX,RCY,sx,sy,COL_SWEEP);
    for(uint8_t i=0;i<st->threat_count;i++){
        uint16_t rr=radar_rssi_to_radius(st->threats[i].best_rssi,RR/4,RR);
        uint16_t an=radar_hash_to_angle(st->threats[i].hash);
        int x,y; radar_polar_to_xy(RCX,RCY,rr,an,&x,&y);
        uint16_t col = st->threats[i].kind==DETECT_KIND_KNOWN ? 0xFD20   // amber = known device class
                                                             : threat_color(st->threats[i].epochs);
        radar_gfx_fill_rect(g,x-2,y-2,5,5,col); }
    char b[24];
    if(st->threat_count==0) radar_gfx_text(g,84,250,"CLEAR",COL_OK);
    else { snprintf(b,sizeof b,"! %u FOLLOWERS",(unsigned)st->threat_count); radar_gfx_text(g,40,250,b,COL_WARN); }
    char l[40]; snprintf(l,sizeof l,"decoys %u  up %lus",(unsigned)st->active_devices,(unsigned long)st->uptime_s);
    radar_gfx_text(g,10,296,l,COL_DIM);
}
static void draw_detail(radar_gfx_t *g, const radar_wire_status_t *st){
    radar_gfx_text(g,8,6,"FOLLOWERS",COL_FG);
    if(st->threat_count==0){ radar_gfx_text(g,8,30,"none confirmed",COL_DIM); return; }
    for(uint8_t i=0;i<st->threat_count;i++){ char r[40];
        if(st->threats[i].kind==DETECT_KIND_KNOWN){
            const char *q = st->threats[i].confidence>=80 ? "likely" : "possible";
            snprintf(r,sizeof r,"%s (%s) %ddB",sig_class_name(st->threats[i].class_id),q,
                     (int)st->threats[i].best_rssi);
            radar_gfx_text(g,6,30+i*18,r,0xFD20);   // amber = known device class
        } else {
            snprintf(r,sizeof r,"%08lx v%04x %ddB %uep",(unsigned long)st->threats[i].hash,
                     (unsigned)st->threats[i].vendor,(int)st->threats[i].best_rssi,(unsigned)st->threats[i].epochs);
            radar_gfx_text(g,6,30+i*18,r,threat_color(st->threats[i].epochs));
        } } }
static void draw_stats(radar_gfx_t *g, const radar_wire_status_t *st){
    char l[40]; int y=6; radar_gfx_text(g,8,y,"DECOY / POP",COL_FG); y+=24;
    #define ROW(...) do{ snprintf(l,sizeof l,__VA_ARGS__); radar_gfx_text(g,6,y,l,COL_DIM); y+=18; }while(0)
    ROW("decoys %u/%u tgt %u",(unsigned)st->active_devices,(unsigned)st->roster_size,(unsigned)st->active_target);
    ROW("pop %u obs %lu",(unsigned)st->pop_ewma,(unsigned long)st->total_obs);
    ROW("epoch %u probes %lu",(unsigned)st->epoch,(unsigned long)st->probes_sent);
    ROW("churn %s",(st->flags&0x1)?"PAUSED":"run");
    ROW("up %lus",(unsigned long)st->uptime_s);
    #undef ROW
}
static void fmt_age(char *out, size_t n, const char *label, uint32_t age_s){
    if (age_s == UINT32_MAX) snprintf(out, n, "%s never", label);
    else                     snprintf(out, n, "%s %lus ago", label, (unsigned long)age_s);
}
static void draw_library(radar_gfx_t *g, const radar_lib_info_t *lib){
    char l[40]; int y=6; radar_gfx_text(g,8,y,"LIBRARY",COL_FG); y+=24;
    #define ROW(...) do{ snprintf(l,sizeof l,__VA_ARGS__); radar_gfx_text(g,6,y,l,COL_DIM); y+=18; }while(0)
    if (!lib) { radar_gfx_text(g,6,y,"not a librarian",COL_DIM); return; }
    if (lib->sd_ok) ROW("sd OK %luMB",(unsigned long)lib->card_mb);
    else            radar_gfx_text(g,6,y,"sd ABSENT (RAM only)",COL_WARN), y+=18;
    ROW("lib %u/%u shapes",(unsigned)lib->lib_count,(unsigned)lib->lib_cap);
    fmt_age(l,sizeof l,"offer rx",lib->offer_age_s); radar_gfx_text(g,6,y,l,COL_DIM); y+=18;
    fmt_age(l,sizeof l,"sync tx ",lib->sync_age_s);  radar_gfx_text(g,6,y,l,COL_DIM); y+=18;
    if (lib->save_age_s == UINT32_MAX) ROW("save never");
    else ROW("save %lus ago (%luB)",(unsigned long)lib->save_age_s,(unsigned long)lib->save_bytes);
    #undef ROW
}
void radar_render_view(radar_view_t view, const radar_wire_status_t *st, const radar_lib_info_t *lib,
                       uint16_t sweep, uint16_t *band, int band_h, int w, int h, radar_flush_fn flush, void *ctx){
    for(int y0=0;y0<h;y0+=band_h){ radar_gfx_t g={ .buf=band, .w=w, .y0=y0, .h=band_h };
        radar_gfx_clear(&g,COL_BG);
        if(view==RADAR_VIEW_DETAIL) draw_detail(&g,st);
        else if(view==RADAR_VIEW_STATS) draw_stats(&g,st);
        else if(view==RADAR_VIEW_LIBRARY) draw_library(&g,lib);
        else draw_radar(&g,st,sweep);
        flush(y0, band_h, band, ctx); }
}
