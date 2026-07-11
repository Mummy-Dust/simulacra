#include <stdio.h>
#include <string.h>
#include "radar_ui.h"
static const char *N[] = {"HOME","RADAR","DETAIL","STATS","LIBRARY","CONTROL","INFO"};
int main(int argc, char **argv){
    radar_ui_t ui; uint32_t t = 0; uint8_t threats = 0;
    for (int i = 1; i < argc; i++){
        if(!strcmp(argv[i],"reset")) radar_ui_reset(&ui,t,threats);
        else if(!strcmp(argv[i],"input")) radar_ui_on_input(&ui,t);
        else if(!strcmp(argv[i],"select")){ int v=atoi(argv[++i]); radar_ui_select_view(&ui,(radar_view_t)v,t); }
        else if(!strcmp(argv[i],"idle")){ t += RADAR_VIEW_IDLE_MS + 1; radar_ui_on_tick(&ui,t,threats); }
        else if(!strcmp(argv[i],"idle_tick")){ t += RADAR_VIEW_IDLE_MS + 1; radar_ui_on_tick(&ui,t,threats); }
        else if(!strcmp(argv[i],"follower1")){ threats = 1; radar_ui_on_tick(&ui,t,threats); }
        printf("%s\n", N[ui.view]);
    }
    return 0;
}
