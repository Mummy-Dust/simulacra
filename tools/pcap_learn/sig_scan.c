/* capture signature scan: runs the REAL tracker/surveillance matcher (sig_match.c +
 * sig_seed.c) over adverts (NDJSON from parse_pcap.py), and does per-address dwell
 * analysis. Validates/refines the seeded signatures against real-world data and shows
 * how transient a moving capture is vs. what a stationary capture would reveal.
 *
 * Build: make sig_scan
 * Run:   ./sig_scan adverts.ndjson
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "threat_sig.h"
#include "sig_match.h"
#include "sig_seed.h"

static const char *CLASS_NAME[] = { "AirTag", "SmartTag", "Tile" };

#define MAXADDR 32768
typedef struct { char addr[16]; uint8_t class_id; unsigned count; double first, last; } dwell_t;
static dwell_t dw[MAXADDR];
static size_t dwn;

static dwell_t *dwell_get(const char *addr, uint8_t class_id){
    for (size_t i = 0; i < dwn; i++) if (strcmp(dw[i].addr, addr) == 0) return &dw[i];
    if (dwn >= MAXADDR) return NULL;
    dwell_t *d = &dw[dwn++];
    snprintf(d->addr, sizeof d->addr, "%s", addr);
    d->class_id = class_id; d->count = 0; d->first = 1e18; d->last = -1e18;
    return d;
}

/* extract a quoted string value: key must be like "\"addr\":\"" ; returns len, fills out */
static int getstr(const char *line, const char *keyq, char *out, int cap){
    const char *p = strstr(line, keyq);
    if (!p) { out[0] = 0; return 0; }
    p += strlen(keyq);
    const char *e = strchr(p, '"');
    if (!e) { out[0] = 0; return 0; }
    int n = (int)(e - p); if (n > cap - 1) n = cap - 1;
    memcpy(out, p, n); out[n] = 0; return n;
}
static long getint(const char *line, const char *key){
    const char *p = strstr(line, key);
    return p ? strtol(p + strlen(key), NULL, 10) : 0;
}
static double getdbl(const char *line, const char *key){
    const char *p = strstr(line, key);
    return p ? strtod(p + strlen(key), NULL) : 0.0;
}
static int hexval(int c){ return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static uint8_t unhex(const char *s, uint8_t *out, int cap){
    int n = 0; while (s[0] && s[1] && n < cap){ out[n++] = (uint8_t)((hexval(s[0])<<4)|hexval(s[1])); s += 2; }
    return (uint8_t)n;
}
static uint8_t atype_bit(const char *a){
    if (!strcmp(a,"public")) return SIG_ADDR_PUBLIC;
    if (!strcmp(a,"static")) return SIG_ADDR_STATIC;
    if (!strcmp(a,"rpa"))    return SIG_ADDR_RPA;
    if (!strcmp(a,"nrpa"))   return SIG_ADDR_NRPA;
    return 0;
}

int main(int argc, char **argv){
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f){ fprintf(stderr, "cannot open input\n"); return 1; }

    threat_sig_t db[SIG_DB_CAP];
    size_t ndb = sig_seed_copy(db, SIG_DB_CAP);

    char line[2048];
    unsigned adverts = 0, apple = 0, hits = 0;
    unsigned cls_hits[SIG_CLASS_COUNT] = {0};
    while (fgets(line, sizeof line, f)){
        if (!strstr(line, "\"ad\":")) continue;
        adverts++;
        long company = getint(line, "\"company\":");
        long svc = getint(line, "\"svc\":");
        double ts = getdbl(line, "\"ts\":");
        char atype[8], addr[16], mfghex[128], svchex[128];
        getstr(line, "\"atype\":\"", atype, sizeof atype);
        getstr(line, "\"addr\":\"", addr, sizeof addr);
        getstr(line, "\"mfg\":\"", mfghex, sizeof mfghex);
        getstr(line, "\"svcd\":\"", svchex, sizeof svchex);
        if (company == 0x004C) apple++;

        uint8_t mfg[64], svcd[64];
        sig_adv_fields_t adv = {0};
        adv.company_id = (uint16_t)(mfghex[0] ? company : 0xFFFF);
        adv.svc_uuid16 = (uint16_t)svc;
        adv.addr_type  = atype_bit(atype);
        adv.mfg_len = mfghex[0] ? unhex(mfghex, mfg, sizeof mfg) : 0;
        adv.mfg_data = adv.mfg_len ? mfg : NULL;
        adv.svc_len = svchex[0] ? unhex(svchex, svcd, sizeof svcd) : 0;
        adv.svc_data = adv.svc_len ? svcd : NULL;

        sig_hit_t hit;
        if (sig_match(&adv, db, ndb, &hit)){
            hits++;
            if (hit.class_id < SIG_CLASS_COUNT) cls_hits[hit.class_id]++;
            dwell_t *d = dwell_get(addr, hit.class_id);
            if (d){ d->count++; if (ts < d->first) d->first = ts; if (ts > d->last) d->last = ts; }
        }
    }

    /* dwell stats over matched addresses */
    unsigned once = 0, multi = 0; double maxdwell = 0; const dwell_t *maxd = NULL;
    for (size_t i = 0; i < dwn; i++){
        if (dw[i].count <= 1) once++; else multi++;
        double span = dw[i].last - dw[i].first;
        if (span > maxdwell){ maxdwell = span; maxd = &dw[i]; }
    }

    printf("=== capture signature scan ===\n");
    printf("adverts scanned   : %u\n", adverts);
    printf("signature DB      : %zu sigs (v%u): AirTag / SmartTag / Tile\n", ndb, sig_seed_version());
    printf("tracker hits      : %u  (unique addresses: %zu)\n", hits, dwn);
    for (int c = 0; c < SIG_CLASS_COUNT; c++)
        printf("  %-9s: %u\n", CLASS_NAME[c], cls_hits[c]);
    printf("selectivity       : Apple adverts %u -> AirTag hits %u (%.2f%%)\n",
           apple, cls_hits[0], apple ? 100.0 * cls_hits[0] / apple : 0.0);
    printf("dwell (matched addrs): seen-once %u, seen-multi %u, max-dwell %.1fs%s%s\n",
           once, multi, maxdwell,
           maxd ? "  " : "", maxd ? maxd->addr : "");
    printf("top matched by sightings:\n");
    for (int shown = 0; shown < 8; shown++){
        int bi = -1;
        for (size_t i = 0; i < dwn; i++){
            if (dw[i].count == 0xFFFFFFFF) continue;
            if (bi < 0 || dw[i].count > dw[bi].count) bi = (int)i;
        }
        if (bi < 0 || dw[bi].count == 0) break;
        printf("  %s  %-9s  x%u  dwell %.1fs\n",
               dw[bi].addr, CLASS_NAME[dw[bi].class_id], dw[bi].count, dw[bi].last - dw[bi].first);
        dw[bi].count = 0xFFFFFFFF;
    }
    printf("note: a moving capture makes most sightings transient (dwell ~0); telling a follower\n");
    printf("      from a passerby needs a stationary/route capture (persistence) or live on-device.\n");
    return 0;
}
