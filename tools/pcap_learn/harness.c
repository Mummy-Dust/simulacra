/* pcap-learn harness: replays real BLE adverts (NDJSON from parse_pcap.py) through the
 * actual firmware pipeline (learn_strip -> learn_shape_hash -> learn_merge), prints a
 * validation report incl. a structure-only audit, and emits a plaintext learn.seed.
 *
 * Build: make   (compiles the real law3.c / learn_wire.c / learn.c against host_stubs/)
 * Run:   ./harness adverts.ndjson
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "learn.h"          /* learn_strip */
#include "learn_record.h"   /* learned_template_t, learn_shape_hash */
#include "learn_wire.h"     /* learn_merge, learn_top_n, learn_regate */
#include "law3.h"           /* law3_forbidden (reject-reason classification) */

#define CAP     4096        /* generous offline store: no eviction during analysis */
#define SEED_N  128         /* seed = top-N by reinforce (Ward cap) */

static learned_template_t store[CAP];
static size_t count;

static int hexval(int c){ return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

/* Structure-only audit for one record: count identity bytes not accounted for.
 * A value byte is OK if it is a preserved structural field, masked (rewritten per
 * render), or inside the recorded local-name region (replaced at render). Anything
 * else is an identity LEAK. Returns leaks; adds retained-name bytes to *named_out. */
static int audit_record(const learned_template_t *r, int *named_out){
    int leak = 0;
    for (uint8_t i = 0; i + 1 < r->ad_len; ){
        uint8_t l = r->ad[i];
        if (l == 0 || i + 1 + l > r->ad_len) break;
        uint8_t type = r->ad[i + 1], vfrom = i + 2, vto = i + 1 + l;
        for (uint8_t b = vfrom; b < vto; b++){
            int preserved = 0;
            int masked = (r->rand_mask >> b) & 1;
            int named  = r->name_len && b >= r->name_off && b < r->name_off + r->name_len;
            switch (type){
                case 0x01: case 0x02: case 0x03: case 0x0A: case 0x19:
                    preserved = 1; break;                          /* flags/uuid16/tx/appearance */
                case 0xFF:
                    if (b < vfrom + 2) preserved = 1;              /* company id */
                    else if (r->ad[vfrom] == 0x4C && r->ad[vfrom+1] == 0x00 &&
                             (vto - vfrom) >= 4 && r->ad[vfrom+2] == 0x02 &&
                             r->ad[vfrom+3] == 0x15 && b < vfrom + 4) preserved = 1; /* iBeacon prefix */
                    break;
                case 0x16:
                    if (b < vfrom + 2) preserved = 1; break;       /* svc-data uuid */
                default: break;                                    /* unknown: must be masked */
            }
            if (preserved || masked) continue;
            if (named) { (*named_out)++; continue; }
            leak++;
        }
        i += 1 + l;
    }
    return leak;
}

/* Zero the local-name value bytes (kept verbatim by learn_strip, replaced only at render)
 * so a seed derived from real devices carries no bystander names. name_off/name_len stay,
 * so on-device render still writes a synthetic name. shape_hash is unaffected (it hashes
 * only family/company/svc + the AD type/length sequence, never value bytes). */
static void scrub_name(learned_template_t *r){
    for (uint8_t b = r->name_off; r->name_len && b < r->name_off + r->name_len && b < 31; b++)
        r->ad[b] = 0;
}

/* ---- advertising-interval enrichment ---------------------------------------
 * Mirror the firmware's on-device estimator (learn.c: gap between consecutive
 * sightings of the SAME device, kept if 20..60000 ms, tracking min & max). We
 * group by advertiser address here, aggregate per shape, and populate the seed's
 * interval band -- so seeded decoys advertise at a plausible cadence for their
 * device type instead of the generic 100-300 ms fallback (a timing tell). NOTE:
 * a moving capture gives few sightings/device (and RPA addresses rotate), so most
 * shapes fall back to a family default; a STATIONARY capture yields far better
 * estimates. Same-device ADV_IND vs SCAN_RSP differ, so a device's interval is
 * attributed to its last-seen shape -- fine in aggregate. */
#define IV_SLOTS     (1<<18)
#define IV_MINGAP    20      /* ms: below this = same-event multi-channel burst, not an interval */
#define IV_MAXGAP    60000   /* ms: above this the device left (separate encounter) */
#define IV_SHAPE_MIN 5       /* shapes with >= this many timed devices use the estimate, else default */

typedef struct { uint8_t addr[6], used; uint32_t last_ms; uint16_t gmin, nsight; uint32_t shape; } iv_dev_t;
static iv_dev_t iv[IV_SLOTS];
static uint32_t iv_hash(const uint8_t a[6]){
    uint32_t h = 2166136261u; for (int i = 0; i < 6; i++){ h ^= a[i]; h *= 16777619u; } return h & (IV_SLOTS - 1);
}
static iv_dev_t *iv_get(const uint8_t a[6]){
    uint32_t s = iv_hash(a);
    for (uint32_t p = 0; p < IV_SLOTS; p++){
        iv_dev_t *d = &iv[(s + p) & (IV_SLOTS - 1)];
        if (!d->used){ memcpy(d->addr, a, 6); d->used = 1; return d; }
        if (memcmp(d->addr, a, 6) == 0) return d;
    }
    return NULL;
}

/* Per shape: collect each contributing device's MINIMUM observed gap (~= its advertising
 * interval), then use the MEDIAN across devices for a robust central estimate (per-device min
 * is noisy -- occasional ~20ms multi-channel artifacts or inflated values when the sniffer
 * caught nothing consecutive; the median shrugs both off). Band = a tight spread around it. */
#define IV_SAMPLE 64
typedef struct { uint32_t shape; uint16_t mins[IV_SAMPLE]; uint8_t nmins; uint32_t ndev; } iv_shape_t;
static iv_shape_t ivs[CAP];
static size_t ivsn;
static iv_shape_t *ivs_get(uint32_t shape){          /* find-or-create */
    for (size_t i = 0; i < ivsn; i++) if (ivs[i].shape == shape) return &ivs[i];
    if (ivsn >= CAP) return NULL;
    iv_shape_t *e = &ivs[ivsn++]; e->shape = shape; e->nmins = 0; e->ndev = 0; return e;
}
static int u16cmp(const void *a, const void *b){ return (int)*(const uint16_t*)a - (int)*(const uint16_t*)b; }
static iv_shape_t *ivs_find(uint32_t shape){         /* find, no create */
    for (size_t i = 0; i < ivsn; i++) if (ivs[i].shape == shape) return &ivs[i];
    return NULL;
}
static void family_default(uint8_t fam, uint16_t *lo, uint16_t *hi){
    switch (fam){
        case 1:          *lo = 100;  *hi = 900;  break;  /* iBeacon */
        case 2: case 3:  *lo = 100;  *hi = 1000; break;  /* Eddystone UID/URL */
        case 4:          *lo = 1000; *hi = 3000; break;  /* svc tracker (Tile-like, slow) */
        default:         *lo = 200;  *hi = 1500; break;  /* vendor-mfg / mixed */
    }
}
static uint16_t clamp_iv(long v){ if (v < IV_MINGAP) v = IV_MINGAP; if (v > 10240) v = 10240; return (uint16_t)v; }

int main(int argc, char **argv){
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f){ fprintf(stderr, "cannot open input\n"); return 1; }

    char line[1024];
    unsigned in = 0, ok = 0, rej = 0, rej_law3 = 0, rej_other = 0;
    uint16_t sweep = 0;
    while (fgets(line, sizeof line, f)){
        char *cp = strstr(line, "\"company\":");
        char *ap = strstr(line, "\"ad\":\"");
        if (!cp || !ap) continue;
        unsigned company = (unsigned)strtoul(cp + 10, NULL, 10);
        char *hex = ap + 6, *end = strchr(hex, '"');
        if (!end) continue;
        size_t hlen = (size_t)(end - hex);
        if (hlen % 2) continue;
        uint8_t ad[64]; size_t adlen = hlen / 2;
        if (adlen > 31) adlen = 31;
        for (size_t i = 0; i < adlen; i++) ad[i] = (uint8_t)((hexval(hex[2*i]) << 4) | hexval(hex[2*i+1]));
        in++;

        learned_template_t rec; memset(&rec, 0, sizeof rec);
        if (!learn_strip(ad, (uint8_t)adlen, (uint16_t)company, &rec)){
            rej++;
            if (law3_forbidden(ad, (uint8_t)adlen)) rej_law3++; else rej_other++;
            continue;
        }
        rec.shape_hash = learn_shape_hash(&rec);
        learn_merge(store, &count, CAP, &rec, sweep++);
        ok++;

        /* interval enrichment: fold this sighting into its device's gap stats */
        char *tp = strstr(line, "\"ts\":");
        char *dp = strstr(line, "\"addr\":\"");
        if (tp && dp && strlen(dp + 8) >= 12){
            double ts = strtod(tp + 5, NULL);
            uint8_t a[6];
            for (int i = 0; i < 6; i++) a[i] = (uint8_t)((hexval(dp[8 + 2*i]) << 4) | hexval(dp[8 + 2*i + 1]));
            uint32_t now_ms = (uint32_t)(ts * 1000.0);
            iv_dev_t *d = iv_get(a);
            if (d){
                if (d->nsight > 0){
                    uint32_t gap = now_ms - d->last_ms;          /* modular; ok for gaps < ~49 days */
                    if (gap >= IV_MINGAP && gap < IV_MAXGAP){
                        uint16_t g = (uint16_t)gap;
                        if (d->gmin == 0 || g < d->gmin) d->gmin = g;
                    }
                }
                d->last_ms = now_ms;
                if (d->nsight < 0xFFFF) d->nsight++;
                d->shape = rec.shape_hash;
            }
        }
    }

    /* aggregate per-device gap stats into per-shape interval bands (mirror firmware:
     * a device counts only after >=3 sightings, like LEARN_MIN_SIGHTINGS) */
    for (size_t i = 0; i < IV_SLOTS; i++){
        if (!iv[i].used || iv[i].nsight < 3 || iv[i].gmin == 0) continue;
        iv_shape_t *e = ivs_get(iv[i].shape);
        if (!e) continue;
        if (e->nmins < IV_SAMPLE) e->mins[e->nmins++] = iv[i].gmin;
        e->ndev++;
    }

    /* structure-only audit over the deduped store */
    int total_leak = 0, worst = 0, named_bytes = 0;
    for (size_t i = 0; i < count; i++){
        int nb = 0, lk = audit_record(&store[i], &nb);
        total_leak += lk; named_bytes += nb;
        if (lk > worst) worst = lk;
    }

    /* seed = top-N by reinforce; scrub names; re-gate exactly as the device will */
    static learned_template_t seed[SEED_N];
    size_t sn = learn_top_n(store, count, seed, SEED_N);
    int regate_fail = 0; unsigned iv_estimated = 0, iv_default = 0;
    for (size_t i = 0; i < sn; i++){
        scrub_name(&seed[i]);
        /* interval band: measured from timing if the shape has enough timed devices, else family default */
        iv_shape_t *e = ivs_find(seed[i].shape_hash);
        uint16_t lo, hi;
        if (e && e->ndev >= IV_SHAPE_MIN){
            qsort(e->mins, e->nmins, sizeof e->mins[0], u16cmp);
            long med = e->mins[e->nmins / 2];           /* robust central interval */
            lo = clamp_iv(med * 2 / 3);                 /* tight, realistic spread around it */
            hi = clamp_iv(med * 3 / 2);
            iv_estimated++;
        } else {
            family_default(seed[i].family, &lo, &hi);
            iv_default++;
        }
        if (hi < lo) hi = lo;
        seed[i].itvl_min_ms = lo; seed[i].itvl_max_ms = hi;
        if (!learn_regate(&seed[i])) regate_fail++;
    }

    printf("=== pcap-learn report ===\n");
    printf("adverts in        : %u\n", in);
    printf("parsed (stripped) : %u\n", ok);
    printf("rejected          : %u  (law3=%u other=%u)\n", rej, rej_law3, rej_other);
    printf("unique shapes     : %zu\n", count);
    printf("structure audit   : %s (%d leaked identity bytes, worst rec=%d)\n",
           total_leak ? "FAIL" : "PASS", total_leak, worst);
    printf("retained-name bytes: %d (kept by strip for render; scrubbed in seed)\n", named_bytes);

    /* top 10 shapes by reinforce_count, via a shown[] bitmap (no clobber) */
    printf("top shapes (family/company/svc/adlen x reinforce):\n");
    static unsigned char shown[CAP];
    for (int n = 0; n < 10; n++){
        int bi = -1;
        for (size_t i = 0; i < count; i++){
            if (shown[i]) continue;
            if (bi < 0 || store[i].reinforce_count > store[bi].reinforce_count) bi = (int)i;
        }
        if (bi < 0) break;
        shown[bi] = 1;
        learned_template_t *t = &store[bi];
        printf("  fam=%u company=0x%04X svc=0x%04X adlen=%2u  x%u\n",
               t->family, t->company_id, t->svc_uuid, t->ad_len, t->reinforce_count);
    }

    /* emit seed. Serialize each record explicitly in the 55-byte packed little-endian
     * layout of learned_template_t, so the file is device-correct regardless of how the
     * HOST compiler packs the struct (an MSVC build without __attribute__((packed)) would
     * otherwise emit padded 60-byte records the firmware's packed fread cannot consume). */
    printf("seed records      : %zu  (regate_fail=%d)\n", sn, regate_fail);
    printf("interval enrichment: %u from timing (>=%d devices), %u family-default\n",
           iv_estimated, IV_SHAPE_MIN, iv_default);
    FILE *sf = fopen("learn.seed", "wb");
    if (sf){
        uint32_t magic = 0x4C534431u;  /* "LSD1" */
        uint16_t ver = 1, cnt = (uint16_t)sn;
        fwrite(&magic, 4, 1, sf); fwrite(&ver, 2, 1, sf); fwrite(&cnt, 2, 1, sf);
        for (size_t i = 0; i < sn; i++){
            const learned_template_t *t = &seed[i];
            unsigned char b[55]; int p = 0;
            memcpy(b + p, t->ad, 31); p += 31;
            b[p++] = t->ad_len; b[p++] = t->name_off; b[p++] = t->name_len;
            b[p++] = t->rand_mask & 0xFF;       b[p++] = (t->rand_mask >> 8) & 0xFF;
            b[p++] = (t->rand_mask >> 16)&0xFF;  b[p++] = (t->rand_mask >> 24)&0xFF;
            b[p++] = t->company_id & 0xFF;      b[p++] = (t->company_id >> 8) & 0xFF;
            b[p++] = t->svc_uuid & 0xFF;        b[p++] = (t->svc_uuid >> 8) & 0xFF;
            b[p++] = t->family;
            b[p++] = t->itvl_min_ms & 0xFF;     b[p++] = (t->itvl_min_ms >> 8) & 0xFF;
            b[p++] = t->itvl_max_ms & 0xFF;     b[p++] = (t->itvl_max_ms >> 8) & 0xFF;
            b[p++] = t->shape_hash & 0xFF;      b[p++] = (t->shape_hash >> 8) & 0xFF;
            b[p++] = (t->shape_hash >> 16)&0xFF; b[p++] = (t->shape_hash >> 24)&0xFF;
            b[p++] = t->reinforce_count & 0xFF; b[p++] = (t->reinforce_count >> 8) & 0xFF;
            b[p++] = t->last_seen_sweep & 0xFF; b[p++] = (t->last_seen_sweep >> 8) & 0xFF;
            fwrite(b, 1, 55, sf);
        }
        fclose(sf);
        printf("wrote learn.seed  : %zu bytes (%zu-byte records)\n", 8 + sn * 55, (size_t)55);
    }

    if (total_leak) return 2;      /* structure-only violation */
    if (regate_fail) return 3;     /* device would reject these seed records */
    return 0;
}
