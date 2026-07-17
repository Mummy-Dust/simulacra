#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe_frame.h"
#include "probe_agents.h"
#include "uniq_id.h"
#include "phantom.h"

/*
 * Host dumper for the probe-request archetype builder.
 *
 *   probe_dump <arch_idx> <channel> <band5:0|1>   -> one hex line: the built frame
 *   probe_dump --pick <seed> <n>                  -> n lines, each a picked archetype index
 *
 * A fixed source MAC keeps frame output deterministic for byte-exact fixtures.
 */
int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--routecheck") == 0) {
        srand(argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1);
        uniq_reset();
        uint8_t m[6];
        probe_random_mac(m);
        printf("%d\n", uniq_try(m) ? 1 : 0);   // 0 = routed (recorded), 1 = not routed
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--uniq") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 1000;
        srand(seed);
        uniq_reset();
        for (int i = 0; i < n; i++) {           // one distinct pass of n addresses
            uint8_t a[6];
            do { for (int b = 0; b < 6; b++) a[b] = (uint8_t)(rand() & 0xff); } while (!uniq_try(a));
            for (int b = 0; b < 6; b++) printf("%02x", a[b]);
            printf("\n");
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--uniqreset") == 0) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 200;
        for (int half = 0; half < 2; half++) {
            srand(seed);
            uniq_reset();
            for (int i = 0; i < n / 2; i++) {
                uint8_t a[6];
                do { for (int b = 0; b < 6; b++) a[b] = (uint8_t)(rand() & 0xff); } while (!uniq_try(a));
                for (int b = 0; b < 6; b++) printf("%02x", a[b]);
                printf("\n");
            }
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--agents") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      nag    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 8;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 2000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 2000;
        srand(seed);
        uint32_t t = 0;
        probe_agents_init(nag, t);
        for (int s = 0; s < ticks; s++) {
            t += tickms;
            probe_agents_lifecycle(t);
            probe_agent_t *due[PROBE_AGENTS_MAX];
            int nd = probe_agents_due(t, due, PROBE_AGENTS_MAX);
            for (int i = 0; i < nd; i++) {
                uint16_t sq = probe_agent_next_seq(due[i]);
                printf("E %u ", (unsigned)t);
                for (int b = 0; b < 6; b++) printf("%02x", due[i]->mac[b]);
                printf(" %u\n", (unsigned)sq);
            }
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--phantoms") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n      = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 12;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 4000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 1000;
        srand(seed);
        uint32_t t = 0;
        phantom_init(n, t);
        static uint32_t gen_seen[PHANTOM_MAX];
        for (int i = 0; i < n && i < PHANTOM_MAX; i++) gen_seen[i] = 0;
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            phantom_lifecycle(t);
            for (int i = 0; i < phantom_count(); i++) {
                const phantom_t *ph = phantom_at(i);
                if (ph->generation != gen_seen[i]) {         // emit on each new life
                    gen_seen[i] = ph->generation;
                    printf("P %u %d %d %d %04x %u\n", (unsigned)t, i, (int)ph->family,
                           (int)phantom_arch(ph->family), (unsigned)phantom_company(ph->family),
                           (unsigned)ph->generation);
                }
            }
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--wbind") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      n      = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 12;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 4000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 1000;
        srand(seed);
        uint32_t t = 0;
        phantom_init(n, t);
        probe_agents_init(n, t);
        phantom_sync_wifi(t);
        static uint32_t gen_seen[PROBE_AGENTS_MAX];
        for (int i = 0; i < n && i < PROBE_AGENTS_MAX; i++) gen_seen[i] = 0;
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            phantom_lifecycle(t);
            phantom_sync_wifi(t);
            for (int i = 0; i < probe_agents_count(); i++) {
                const probe_agent_t *a = probe_agents_at(i);
                if (a->persona_gen != gen_seen[i]) {
                    gen_seen[i] = a->persona_gen;
                    printf("W %u %d ", (unsigned)t, i);
                    for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
                    printf(" %d %u\n", (int)a->arch, (unsigned)a->persona_gen);
                }
            }
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--pick") == 0) {
        srand(argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1);
        int n = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 1000;
        for (int i = 0; i < n; i++) printf("%d\n", (int)probe_pick_archetype());
        return 0;
    }

    probe_arch_t a = (argc > 1) ? (probe_arch_t)strtoul(argv[1], 0, 10) : ARCH_IPHONE;
    unsigned ch    = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 10) : 6;
    bool band5     = (argc > 3) ? (strtoul(argv[3], 0, 10) != 0) : false;

    uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
    if (probe_build_request(mac, (uint8_t)ch, a, band5, f, &n)) {
        fprintf(stderr, "build failed (arch=%u band5=%d)\n", a, band5);
        return 2;
    }
    for (size_t i = 0; i < n; i++) printf("%02x", f[i]);
    printf("\n");
    return 0;
}
