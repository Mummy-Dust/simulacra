#include "radar_gfx.h"
#include "radar_font.h"
#include <stdlib.h>

void radar_gfx_clear(radar_gfx_t *g, uint16_t c){ for(int i=0;i<g->w*g->h;i++) g->buf[i]=c; }
void radar_gfx_pixel(radar_gfx_t *g, int x, int y, uint16_t c){
    if(x<0||x>=g->w){ return; }
    int ry=y-g->y0;
    if(ry<0||ry>=g->h){ return; }
    g->buf[ry*g->w+x]=c; }
void radar_gfx_hline(radar_gfx_t *g,int x0,int x1,int y,uint16_t c){ for(int x=x0;x<=x1;x++) radar_gfx_pixel(g,x,y,c); }
void radar_gfx_vline(radar_gfx_t *g,int x,int y0,int y1,uint16_t c){ for(int y=y0;y<=y1;y++) radar_gfx_pixel(g,x,y,c); }
void radar_gfx_fill_rect(radar_gfx_t *g,int x0,int y0,int w,int h,uint16_t c){
    for(int y=y0;y<y0+h;y++) for(int x=x0;x<x0+w;x++) radar_gfx_pixel(g,x,y,c); }
void radar_gfx_line(radar_gfx_t *g,int x0,int y0,int x1,int y1,uint16_t c){
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
    for(;;){ radar_gfx_pixel(g,x0,y0,c); if(x0==x1&&y0==y1)break; int e2=2*err;
        if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} } }
void radar_gfx_circle(radar_gfx_t *g,int cx,int cy,int r,uint16_t c){
    int x=r,y=0,e=1-r; while(x>=y){
        radar_gfx_pixel(g,cx+x,cy+y,c);radar_gfx_pixel(g,cx+y,cy+x,c);radar_gfx_pixel(g,cx-y,cy+x,c);radar_gfx_pixel(g,cx-x,cy+y,c);
        radar_gfx_pixel(g,cx-x,cy-y,c);radar_gfx_pixel(g,cx-y,cy-x,c);radar_gfx_pixel(g,cx+y,cy-x,c);radar_gfx_pixel(g,cx+x,cy-y,c);
        y++; if(e<0)e+=2*y+1; else{x--;e+=2*(y-x)+1;} } }
static void gfx_char(radar_gfx_t *g,int x,int y,char ch,uint16_t c){
    if(ch<0x20||ch>0x7F){ ch='?'; }
    const uint8_t *gl=RADAR_FONT8X8[ch-0x20];
    for(int r=0;r<8;r++) for(int col=0;col<8;col++) if(gl[r]&(1<<col)) radar_gfx_pixel(g,x+col,y+r,c); }
void radar_gfx_text(radar_gfx_t *g,int x,int y,const char *s,uint16_t c){
    for(; *s; s++, x+=8) gfx_char(g,x,y,*s,c); }
