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
int main(int argc, char **argv) {
    unsigned seed = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 10) : 1;
    size_t   n    = (argc > 2) ? (size_t)strtoul(argv[2], 0, 10) : 64;
    srand(seed);
    rf_model_t m; memset(&m, 0, sizeof(m));
    m.magic = RF_MODEL_MAGIC; m.version = RF_MODEL_VERSION;
    /* hardcoded two-vendor model so the generator has something to sample (Task 2: real seed) */
    m.vendors[0].company_id = 0x0075; m.vendors[0].count = 30;
    m.vendors[0].itvl_bins[2] = 30;   /* 100-200ms bin */
    m.vendors[1].company_id = 0x004C; m.vendors[1].count = 10;
    m.vendors[1].itvl_bins[2] = 10;
    m.other_count = 10; m.other_itvl_bins[3] = 10;
    m.total_obs = 50; m.pop_ewma = 12.0f;
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
