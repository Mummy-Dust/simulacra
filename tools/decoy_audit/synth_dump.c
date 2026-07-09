#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rf_model.h"
#include "identity.h"
#include "generate.h"

static const char *atype_of(const uint8_t addr[6]) {
    switch (addr[5] >> 6) { case 3: return "static"; case 1: return "rpa";
                            case 0: return "public"; default: return "nrpa"; }
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
int main(int argc, char **argv) {
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
    if (n > 256) n = 256;
    static identity_t roster[256];
    generate_roster(&m, roster, n);
    for (size_t i = 0; i < n; i++) {
        identity_t *id = &roster[i];
        char hex[13];
        for (int b = 0; b < 6; b++) sprintf(hex + b*2, "%02x", id->addr[b]);
        printf("{\"addr\":\"%s\",\"atype\":\"%s\",\"company\":%u,\"itvl_ms\":%u,"
               "\"tx\":%d,\"arch\":%u,\"plen\":%u}\n",
               hex, atype_of(id->addr), id->company_id, id->adv_itvl_ms,
               id->tx_power, id->archetype_idx, id->payload_len);
    }
    return 0;
}
