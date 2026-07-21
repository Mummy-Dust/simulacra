#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rf_model.h"
#include "identity.h"
#include "generate.h"
#include "fleet_pop.h"
#include "fleet.h"
#include "ble_devices.h"
#include "roster.h"
#include "learn.h"
#include "learn_record.h"
#include "uniq_id.h"
#include "phantom.h"
#include "probe_agents.h"

static const char *atype_of(const uint8_t addr[6]) {
    switch (addr[5] >> 6) { case 3: return "static"; case 1: return "rpa";
                            case 0: return "public"; default: return "nrpa"; }
}
/* On-air manufacturer company id, parsed from the serialized AD payload EXACTLY as
   capture_profile.py does on the real side (walk the AD structures, first 0xFF element's
   little-endian company). 0 = no manufacturer element (service-data / beacon => no-mfg).
   Measuring the transmitted bytes, not the identity's label, keeps the audit honest:
   a Tile carries only service-data on air, and an iBeacon carries Apple 0x004C. */
static unsigned company_onair(const uint8_t *ad, size_t len) {
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t l = ad[i];
        if (l == 0 || i + 1 + (size_t)l > len) break;
        if (ad[i + 1] == 0xFF && l >= 3) return ad[i + 2] | (ad[i + 3] << 8);
        i += 1 + l;
    }
    return 0;
}
/* Ordered AD element type codes from the serialized payload, as a decoy fingerprinter reads
   them off air (e.g. "01,03,16"). Same TLV walk as company_onair; the host serializer emits
   the fields in NimBLE's canonical order, so this matches the on-air structure. */
static void ad_types_onair(const uint8_t *ad, size_t len, char *out, size_t outsz) {
    size_t i = 0, o = 0; if (outsz) out[0] = 0;
    while (i + 1 < len) {
        uint8_t l = ad[i];
        if (l == 0 || i + 1 + (size_t)l > len) break;
        int w = snprintf(out + o, outsz - o, o ? ",%02x" : "%02x", ad[i + 1]);
        if (w < 0 || (size_t)w >= outsz - o) break;
        o += (size_t)w;
        i += 1 + l;
    }
}
/* Read a model-seed file (produced by capture_profile.py) into an rf_model_t.
   Format: "POP <f>", "V <cid_hex> <count> <b0..b6>", "OTHER <count> <b0..b6>". */
static int load_model_seed(const char *path, rf_model_t *m) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 1;
    char line[256]; size_t v = 0; uint64_t total = 0;
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!strncmp(line, "POP", 3)) { sscanf(line + 3, "%f", &m->pop_ewma); continue; }
        if (!strncmp(line, "OTHER", 5)) {
            uint32_t c, b[7] = {0};
            sscanf(line + 5, "%u %u %u %u %u %u %u %u", &c, &b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6]);
            m->other_count = c; total += c;
            for (int i=0;i<7;i++) m->other_itvl_bins[i] = b[i];
            continue;
        }
        if (line[0] == 'V' && v < RF_VENDOR_SLOTS) {
            unsigned cid, c, b[7] = {0};
            sscanf(line + 1, "%x %u %u %u %u %u %u %u %u", &cid, &c,
                   &b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6]);
            m->vendors[v].company_id = (uint16_t)cid; m->vendors[v].count = c; total += c;
            for (int i=0;i<7;i++) m->vendors[v].itvl_bins[i] = b[i];
            v++;
        }
    }
    fclose(fp);
    m->total_obs = (uint32_t)total;
    return 0;
}
/* Load a pcap_learn LSD1 seed (8-byte header + 55-byte little-endian records, the format
   tools/pcap_learn/harness.c writes) into the real learn store, so generate_roster reproduces
   the learned shapes via learn_render. Returns records added, or -1 on error. */
static int load_learn_seed(const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) return -1;
    unsigned char h[8];
    if (fread(h, 1, 8, fp) != 8) { fclose(fp); return -1; }
    uint32_t magic = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    uint16_t cnt = (uint16_t)(h[6] | (h[7] << 8));
    if (magic != 0x4C534431u) { fclose(fp); return -1; }   // "LSD1"
    int n = 0;
    for (uint16_t i = 0; i < cnt; i++) {
        unsigned char b[55]; if (fread(b, 1, 55, fp) != 55) break;
        learned_template_t t; memset(&t, 0, sizeof t); int p = 0;
        memcpy(t.ad, b + p, 31); p += 31;
        t.ad_len = b[p++]; t.name_off = b[p++]; t.name_len = b[p++];
        t.rand_mask = (uint32_t)b[p] | ((uint32_t)b[p+1] << 8) | ((uint32_t)b[p+2] << 16) | ((uint32_t)b[p+3] << 24); p += 4;
        t.company_id = (uint16_t)(b[p] | (b[p+1] << 8)); p += 2;
        t.svc_uuid   = (uint16_t)(b[p] | (b[p+1] << 8)); p += 2;
        t.family = b[p++];
        t.itvl_min_ms = (uint16_t)(b[p] | (b[p+1] << 8)); p += 2;
        t.itvl_max_ms = (uint16_t)(b[p] | (b[p+1] << 8)); p += 2;
        t.shape_hash = (uint32_t)b[p] | ((uint32_t)b[p+1] << 8) | ((uint32_t)b[p+2] << 16) | ((uint32_t)b[p+3] << 24); p += 4;
        t.reinforce_count = (uint16_t)(b[p] | (b[p+1] << 8)); p += 2;
        t.last_seen_sweep = (uint16_t)(b[p] | (b[p+1] << 8)); p += 2;
        if (learn_store_add(&t, t.last_seen_sweep)) n++;
    }
    fclose(fp);
    return n;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--fleet-share") == 0) {
        int target = argc > 2 ? (int)strtol(argv[2], 0, 10) : 0;
        int k      = argc > 3 ? (int)strtol(argv[3], 0, 10) : 1;
        printf("%d\n", fleet_pop_share_k(target, k));
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--fleet-size") == 0) {
        printf("%d\n", fleet_pop_size());
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--fleetnode") == 0) {
        char line[64], cmd[16], mh[16];
        unsigned u;
        while (fgets(line, sizeof line, stdin)) {
            if (sscanf(line, "%15s", cmd) != 1) continue;
            if (strcmp(cmd, "reset") == 0) {
                fleet_reset();
            } else if (strcmp(cmd, "note") == 0 && sscanf(line, "%*s %12s %u", mh, &u) == 2) {
                uint8_t m[6];
                for (int i = 0; i < 6; i++) { char b[3] = { mh[2 * i], mh[2 * i + 1], 0 }; m[i] = (uint8_t)strtoul(b, 0, 16); }
                fleet_note_peer_node(m, u);
            } else if (strcmp(cmd, "count") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                printf("%d\n", (int)fleet_node_count(u));
            } else if (strcmp(cmd, "livesize") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                printf("%d\n", fleet_pop_live_size(u));
            }
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--persona-pop") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      nph    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        int      ndev   = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 24;
        int      ticks  = argc > 5 ? (int)strtoul(argv[5], 0, 10) : 4000;
        unsigned tickms = argc > 6 ? (unsigned)strtoul(argv[6], 0, 10) : 1000;
        srand(seed);
        roster_init();
        uint32_t t = 0;
        phantom_init(nph, t);
        ble_devices_init(ndev, t);
        probe_agents_init(nph, t);
        phantom_sync_wifi(t);
        phantom_sync_ble(t);
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            phantom_lifecycle(t);
            phantom_sync_wifi(t);
            phantom_sync_ble(t);
            ble_devices_tick(t);
        }
        // Snapshot the standing live BLE population (bound RPA personas + unbound crowd) as NDJSON
        // in the SAME shape the roster synth emits, so the existing scorecard scores it unchanged.
        for (int i = 0; i < ble_devices_count(); i++) {
            const ble_device_t *d = ble_devices_at(i);
            char hex[13];
            for (int b = 0; b < 6; b++) sprintf(hex + b*2, "%02x", d->id.addr[b]);
            char adsig[48]; ad_types_onair(d->id.payload, d->id.payload_len, adsig, sizeof adsig);
            printf("{\"addr\":\"%s\",\"atype\":\"%s\",\"company\":%u,\"itvl_ms\":%u,"
                   "\"tx\":%d,\"arch\":%u,\"plen\":%u,\"ad\":\"%s\"}\n",
                   hex, atype_of(d->id.addr), company_onair(d->id.payload, d->id.payload_len),
                   d->id.adv_itvl_ms, d->id.tx_power, d->id.archetype_idx, d->id.payload_len, adsig);
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--personas") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      nph    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 12;
        int      ndev   = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 24;
        int      ticks  = argc > 5 ? (int)strtoul(argv[5], 0, 10) : 4000;
        unsigned tickms = argc > 6 ? (unsigned)strtoul(argv[6], 0, 10) : 1000;
        srand(seed);
        roster_init();
        uint32_t t = 0;
        phantom_init(nph, t);
        ble_devices_init(ndev, t);        // slots [0,nph) become bound once synced
        probe_agents_init(nph, t);
        phantom_sync_wifi(t);
        phantom_sync_ble(t);
        static uint32_t bgen[PHANTOM_MAX], wgen[PHANTOM_MAX];
        for (int i = 0; i < nph && i < PHANTOM_MAX; i++) { bgen[i] = 0; wgen[i] = 0; }
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            phantom_lifecycle(t);
            phantom_sync_wifi(t);
            phantom_sync_ble(t);
            ble_devices_tick(t);
            for (int i = 0; i < probe_agents_count(); i++) {
                const probe_agent_t *a = probe_agents_at(i);
                if (a->persona_gen != wgen[i]) {
                    wgen[i] = a->persona_gen;
                    printf("W %u %d ", (unsigned)t, i);
                    for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
                    printf(" %d %u\n", (int)a->arch, (unsigned)a->persona_gen);
                }
            }
            for (int i = 0; i < ble_devices_count(); i++) {
                const ble_device_t *d = ble_devices_at(i);
                if (d->persona_idx < 0) continue;               // unbound crowd: not a persona
                int pi = d->persona_idx;
                if (d->persona_gen != bgen[pi]) {
                    bgen[pi] = d->persona_gen;
                    const char *at = d->atype == BLE_ATYPE_STATIC ? "static"
                                   : d->atype == BLE_ATYPE_RPA    ? "rpa" : "nrpa";
                    printf("B %u %d ", (unsigned)t, pi);
                    for (int b = 0; b < 6; b++) printf("%02x", d->id.addr[b]);
                    printf(" %s %04x %u %u\n", at, (unsigned)d->id.company_id,
                           (unsigned)d->persona_gen, (unsigned)d->id.adv_itvl_ms);
                }
            }
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--blerot") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ticks  = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 45;
        unsigned tickms = argc > 4 ? (unsigned)strtoul(argv[4], 0, 10) : 60000;   // 1 min ticks
        srand(seed);
        roster_init();
        uint32_t t = 0;
        ble_devices_init(1, t);
        ble_device_sync(0, 0, 0, t, 2400000u, 1);          // bound slot 0, 40 min life, gen 1
        char last[13] = "";
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            ble_devices_tick(t);
            const ble_device_t *d = ble_devices_at(0);
            char hex[13]; for (int b = 0; b < 6; b++) sprintf(hex + b * 2, "%02x", d->id.addr[b]);
            if (strcmp(hex, last) != 0) { printf("%u %s %u\n", (unsigned)t, hex, (unsigned)d->persona_gen); strcpy(last, hex); }
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--routecheck") == 0) {
        srand(argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1);
        uniq_reset();
        uint8_t a[6];
        make_random_addr(a, 0xc0);
        printf("%d\n", uniq_try(a) ? 1 : 0);   // 0 = routed (recorded), 1 = not routed
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--devices") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ndev   = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 4000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 1000;
        srand(seed);
        roster_init();                                  // build the behaviour library (host: template fallback)
        uint32_t t = 0;
        ble_devices_init(ndev, t);
        static uint8_t prev[BLE_DEVICES_MAX][6];
        static int     seen[BLE_DEVICES_MAX];
        memset(seen, 0, sizeof seen);
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            ble_devices_tick(t);
            int cnt = ble_devices_count();
            for (int i = 0; i < cnt; i++) {
                const ble_device_t *d = ble_devices_at(i);
                if (!seen[i] || memcmp(prev[i], d->id.addr, 6) != 0) {
                    const char *ev = (d->born_ms == t) ? "born" : "rotate";
                    const char *at = d->atype == BLE_ATYPE_STATIC ? "static"
                                   : d->atype == BLE_ATYPE_RPA    ? "rpa" : "nrpa";
                    const char *ro = d->role == BLE_ROLE_PERSISTENT ? "persistent"
                                   : d->role == BLE_ROLE_RESIDENT   ? "resident" : "transient";
                    char hex[13];
                    for (int b = 0; b < 6; b++) sprintf(hex + b*2, "%02x", d->id.addr[b]);
                    printf("D %u %d %s %s %s %s %u %u\n", (unsigned)t, i, hex, at, ro, ev,
                           (unsigned)d->id.company_id, (unsigned)d->id.adv_itvl_ms);
                    memcpy(prev[i], d->id.addr, 6); seen[i] = 1;
                }
            }
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--formcounts") == 0) {
        srand(argc > 2 ? (unsigned)strtoul(argv[2],0,10) : 1);
        int n = argc > 3 ? (int)strtoul(argv[3],0,10) : 16;
        roster_init(); ble_devices_init(n, 0);
        uint8_t r=0,w=0,b=0; ble_devices_form_counts(&r,&w,&b);
        printf("%u %u %u %d\n", r, w, b, ble_devices_count());
        return 0;
    }
    unsigned seed = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 10) : 1;
    size_t   n    = (argc > 2) ? (size_t)strtoul(argv[2], 0, 10) : 64;
    srand(seed);
    rf_model_t m; memset(&m, 0, sizeof(m));
    m.magic = RF_MODEL_MAGIC; m.version = RF_MODEL_VERSION;
    if (argc > 3) {
        if (load_model_seed(argv[3], &m)) { fprintf(stderr, "seed load failed\n"); return 2; }
    } else { /* Task-1 fallback model so the generator has something to sample */
        m.vendors[0].company_id = 0x0075; m.vendors[0].count = 30; m.vendors[0].itvl_bins[2] = 30;
        m.vendors[1].company_id = 0x004C; m.vendors[1].count = 10; m.vendors[1].itvl_bins[2] = 10;
        m.other_count = 10; m.other_itvl_bins[3] = 10; m.total_obs = 50; m.pop_ewma = 12.0f;
    }
    // Optional 4th arg: a pcap_learn learn.seed. Loading it makes learn_count()>0, so
    // generate_roster reproduces the learned shapes (self-learning path) instead of only
    // the built-in templates. Absent -> learn_count()==0, identical to the template-only audit.
    if (argc > 4) {
        int lc = load_learn_seed(argv[4]);
        fprintf(stderr, "learn.seed: %d records loaded (learn_count=%u)\n",
                lc, (unsigned)learn_count());
    }
    if (n > 256) n = 256;
    static identity_t roster[256];
    generate_roster(&m, roster, n);
    for (size_t i = 0; i < n; i++) {
        identity_t *id = &roster[i];
        char hex[13];
        for (int b = 0; b < 6; b++) sprintf(hex + b*2, "%02x", id->addr[b]);
        char adsig[48]; ad_types_onair(id->payload, id->payload_len, adsig, sizeof adsig);
        printf("{\"addr\":\"%s\",\"atype\":\"%s\",\"company\":%u,\"itvl_ms\":%u,"
               "\"tx\":%d,\"arch\":%u,\"plen\":%u,\"ad\":\"%s\"}\n",
               hex, atype_of(id->addr), company_onair(id->payload, id->payload_len), id->adv_itvl_ms,
               id->tx_power, id->archetype_idx, id->payload_len, adsig);
    }
    return 0;
}
