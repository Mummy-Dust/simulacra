#include "learn_record.h"
#include "learn_wire.h"

uint32_t learn_shape_hash(const learned_template_t *t)
{
    uint32_t h = 2166136261u;
    #define FNV(b) do { h ^= (uint8_t)(b); h *= 16777619u; } while (0)
    FNV(t->family); FNV(t->company_id); FNV(t->company_id >> 8);
    FNV(t->svc_uuid); FNV(t->svc_uuid >> 8);
    for (uint8_t i = 0; i + 1 < t->ad_len; ) {           // type/length sequence only
        uint8_t l = t->ad[i];
        if (l == 0 || i + 1 + l > t->ad_len) break;
        FNV(l); FNV(t->ad[i + 1]);
        i += 1 + l;
    }
    #undef FNV
    return h;
}

static int lw_find(const learned_template_t *store, size_t count, uint32_t hash)
{
    for (size_t i = 0; i < count; i++) if (store[i].shape_hash == hash) return (int)i;
    return -1;
}
static size_t lw_weakest(const learned_template_t *store, size_t count)
{
    size_t w = 0;
    for (size_t i = 1; i < count; i++)
        if (store[i].reinforce_count < store[w].reinforce_count ||
            (store[i].reinforce_count == store[w].reinforce_count &&
             store[i].last_seen_sweep < store[w].last_seen_sweep)) w = i;
    return w;
}

bool learn_merge(learned_template_t *store, size_t *count, size_t cap,
                 const learned_template_t *rec, uint16_t sweep_no)
{
    int idx = lw_find(store, *count, rec->shape_hash);
    if (idx >= 0) {
        learned_template_t *e = &store[idx];
        if (e->reinforce_count < 0xFFFF) e->reinforce_count++;
        e->last_seen_sweep = sweep_no;
        if (rec->itvl_min_ms < e->itvl_min_ms) e->itvl_min_ms = rec->itvl_min_ms;
        if (rec->itvl_max_ms > e->itvl_max_ms) e->itvl_max_ms = rec->itvl_max_ms;
        return true;
    }
    if (*count < cap) {
        store[*count] = *rec; store[*count].last_seen_sweep = sweep_no; (*count)++;
        return true;
    }
    size_t w = lw_weakest(store, *count);
    if (rec->reinforce_count < store[w].reinforce_count) return false;
    store[w] = *rec; store[w].last_seen_sweep = sweep_no;
    return true;
}
