#include "fleet.h"
#include <string.h>

static struct { uint8_t mac[6]; uint32_t last_ms; bool used; } s_tbl[FLEET_MAC_CAP];
static struct { uint8_t mac[6]; uint32_t last_ms; bool used; } s_nodes[FLEET_NODE_CAP];

void fleet_reset(void) { memset(s_tbl, 0, sizeof s_tbl); memset(s_nodes, 0, sizeof s_nodes); }

static int fleet_find(const uint8_t mac[6])
{
    for (int i = 0; i < FLEET_MAC_CAP; i++)
        if (s_tbl[i].used && memcmp(s_tbl[i].mac, mac, 6) == 0) return i;
    return -1;
}

void fleet_note_peer_macs(const uint8_t (*macs)[6], size_t n, uint32_t now_ms)
{
    for (size_t k = 0; k < n; k++) {
        int i = fleet_find(macs[k]);
        if (i >= 0) { s_tbl[i].last_ms = now_ms; continue; }   // refresh
        // pick a free slot, else an expired one, else evict the oldest
        int slot = -1, oldest_i = 0; uint32_t oldest = 0;
        for (int j = 0; j < FLEET_MAC_CAP; j++) {
            if (!s_tbl[j].used) { slot = j; break; }
            uint32_t age = now_ms - s_tbl[j].last_ms;
            if (age >= FLEET_MAC_TTL_MS) { slot = j; break; }
            if (age > oldest) { oldest = age; oldest_i = j; }
        }
        if (slot < 0) slot = oldest_i;
        memcpy(s_tbl[slot].mac, macs[k], 6);
        s_tbl[slot].used = true; s_tbl[slot].last_ms = now_ms;
    }
}

bool fleet_mac_excluded(const uint8_t mac[6], uint32_t now_ms)
{
    int i = fleet_find(mac);
    if (i < 0) return false;
    if ((uint32_t)(now_ms - s_tbl[i].last_ms) >= FLEET_MAC_TTL_MS) { s_tbl[i].used = false; return false; }
    return true;
}

size_t fleet_peer_count(uint32_t now_ms)
{
    size_t n = 0;
    for (int i = 0; i < FLEET_MAC_CAP; i++)
        if (s_tbl[i].used && (uint32_t)(now_ms - s_tbl[i].last_ms) < FLEET_MAC_TTL_MS) n++;
    return n;
}

void fleet_note_peer_node(const uint8_t mac[6], uint32_t now_ms)
{
    for (int i = 0; i < FLEET_NODE_CAP; i++)
        if (s_nodes[i].used && memcmp(s_nodes[i].mac, mac, 6) == 0) { s_nodes[i].last_ms = now_ms; return; }
    int slot = -1, oldest_i = 0; uint32_t oldest = 0;
    for (int j = 0; j < FLEET_NODE_CAP; j++) {
        if (!s_nodes[j].used) { slot = j; break; }
        uint32_t age = now_ms - s_nodes[j].last_ms;
        if (age >= FLEET_MAC_TTL_MS) { slot = j; break; }
        if (age > oldest) { oldest = age; oldest_i = j; }
    }
    if (slot < 0) slot = oldest_i;
    memcpy(s_nodes[slot].mac, mac, 6);
    s_nodes[slot].used = true; s_nodes[slot].last_ms = now_ms;
}

size_t fleet_node_count(uint32_t now_ms)
{
    size_t n = 0;
    for (int i = 0; i < FLEET_NODE_CAP; i++)
        if (s_nodes[i].used && (uint32_t)(now_ms - s_nodes[i].last_ms) < FLEET_MAC_TTL_MS) n++;
    return n;
}

size_t fleet_macs_pack(uint8_t *out, size_t out_max, const uint8_t (*macs)[6], size_t n)
{
    if (n > 255) n = 255;
    if (1 + n * 6 > out_max) n = (out_max - 1) / 6;
    out[0] = (uint8_t)n;
    for (size_t k = 0; k < n; k++) memcpy(out + 1 + k * 6, macs[k], 6);
    return 1 + n * 6;
}

size_t fleet_macs_unpack(const uint8_t *in, size_t len, uint8_t (*macs)[6], size_t max)
{
    if (len < 1) return 0;
    size_t n = in[0];
    if (1 + n * 6 > len) return 0;          // malformed
    if (n > max) n = max;
    for (size_t k = 0; k < n; k++) memcpy(macs[k], in + 1 + k * 6, 6);
    return n;
}
