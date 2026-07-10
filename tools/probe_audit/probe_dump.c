#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe_frame.h"

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
