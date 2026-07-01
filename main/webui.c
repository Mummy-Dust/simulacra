#include "webui.h"
#include <stdio.h>
#include <string.h>

int webui_build_status_json(char *buf, size_t len, const webui_status_t *st)
{
    int off = 0, n;
    #define PUT(...) do { \
        n = snprintf(buf + off, len - (size_t)off, __VA_ARGS__); \
        if (n < 0 || (size_t)n >= len - (size_t)off) { \
            if (len) { buf[len - 1] = '\0'; } \
            return -1; \
        } \
        off += n; \
    } while (0)

    PUT("{\"uptime_s\":%u,\"decoy_paused\":%s,\"wifi_config_mode\":%s,"
        "\"active_devices\":%u,\"roster_size\":%u,\"probes_sent\":%u,"
        "\"epoch\":%u,\"pop_ewma\":%u,\"total_obs\":%u,\"active_target\":%u,"
        "\"threat_count\":%u,\"threats\":[",
        (unsigned)st->uptime_s, st->decoy_paused ? "true" : "false",
        st->wifi_config_mode ? "true" : "false",
        (unsigned)st->active_devices, (unsigned)st->roster_size,
        (unsigned)st->probes_sent, (unsigned)st->epoch,
        (unsigned)st->pop_ewma, (unsigned)st->total_obs,
        (unsigned)st->active_target, (unsigned)st->threat_count);

    for (uint8_t i = 0; i < st->threat_count && i < DETECT_MAX_THREATS; i++) {
        const detect_threat_t *t = &st->threats[i];
        PUT("%s{\"hash\":\"%08x\",\"vendor\":%u,\"rssi\":%d,\"epochs\":%u,"
            "\"first\":%u,\"last\":%u}",
            i ? "," : "", (unsigned)t->hash, (unsigned)t->vendor,
            (int)t->best_rssi, (unsigned)t->epochs,
            (unsigned)t->first_epoch, (unsigned)t->last_epoch);
    }
    PUT("]}");
    #undef PUT
    return off;
}
