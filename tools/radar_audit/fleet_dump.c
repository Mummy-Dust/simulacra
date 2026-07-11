#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fleet_status.h"
int main(int argc,char**argv){
    fleet_status_t f; fleet_status_reset(&f); uint32_t t=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"up")){ uint8_t id=atoi(argv[++i]); radar_wire_status_t s; memset(&s,0,sizeof s);
            s.active_devices=atoi(argv[++i]); fleet_status_upsert(&f,id,&s,t); }
        else if(!strcmp(argv[i],"wait")){ t += FLEET_STATUS_STALE_MS + 1; }
        else if(!strcmp(argv[i],"count")) printf("%d\n", fleet_status_count(&f));
        else if(!strcmp(argv[i],"at0")){ uint8_t id; const radar_wire_status_t*st; bool a;
            if(fleet_status_at(&f,0,&id,&st,&a,t)) printf("id=%u dev=%u alive=%d\n",id,st->active_devices,a); }
    }
    return 0;
}
