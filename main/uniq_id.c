#include "uniq_id.h"
#include <string.h>

#ifndef UNIQ_HISTORY
#define UNIQ_HISTORY 2048          // >> max concurrent live identities; ~12 KB
#endif

static uint8_t s_ring[UNIQ_HISTORY][6];
static int     s_count;            // valid entries (caps at UNIQ_HISTORY)
static int     s_head;             // next write slot (oldest when full)

void uniq_reset(void) { s_count = 0; s_head = 0; }

static int in_ring(const uint8_t a[6]) {
    for (int i = 0; i < s_count; i++)
        if (memcmp(s_ring[i], a, 6) == 0) return 1;
    return 0;
}

bool uniq_try(const uint8_t a[6]) {
    if (in_ring(a)) return false;
    memcpy(s_ring[s_head], a, 6);
    s_head = (s_head + 1) % UNIQ_HISTORY;      // wraps -> overwrites the oldest (FIFO eviction)
    if (s_count < UNIQ_HISTORY) s_count++;
    return true;
}
