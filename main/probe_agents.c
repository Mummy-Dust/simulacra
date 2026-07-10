#include "probe_agents.h"
#include "esp_random.h"
#include <string.h>

#define LIFE_MIN_MS    60000u    // 1 min
#define LIFE_MAX_MS    600000u   // 10 min
#define ACTIVE_MIN_MS  4000u
#define ACTIVE_MAX_MS  16000u
#define IDLE_MIN_MS    30000u
#define IDLE_MAX_MS    180000u

static probe_agent_t s_agents[PROBE_AGENTS_MAX];
static int           s_n;

static uint32_t rnd_range(uint32_t lo, uint32_t hi) { return lo + (esp_random() % (hi - lo + 1u)); }

static void agent_spawn(probe_agent_t *a, uint32_t now_ms)
{
    probe_random_mac(a->mac);
    a->arch    = probe_pick_archetype();
    a->seq     = (uint16_t)(esp_random() & 0x0FFFu);             // fresh random 12-bit base
    a->duty    = (esp_random() % 4u == 0u) ? DUTY_ACTIVE : DUTY_IDLE;  // ~25% active
    a->born_ms = now_ms;
    a->life_ms = rnd_range(LIFE_MIN_MS, LIFE_MAX_MS);
    a->alive   = true;
    uint32_t base = (a->duty == DUTY_ACTIVE) ? rnd_range(ACTIVE_MIN_MS, ACTIVE_MAX_MS)
                                             : rnd_range(IDLE_MIN_MS, IDLE_MAX_MS);
    a->next_scan_ms = now_ms + (esp_random() % base);            // random phase-in (not all due at once)
}

void probe_agents_init(int n, uint32_t now_ms)
{
    if (n > PROBE_AGENTS_MAX) n = PROBE_AGENTS_MAX;
    if (n < 1) n = 1;
    s_n = n;
    for (int i = 0; i < s_n; i++) agent_spawn(&s_agents[i], now_ms);
}

uint16_t probe_agent_next_seq(probe_agent_t *a)
{
    uint16_t s = a->seq;
    a->seq = (a->seq + 1u) & 0x0FFFu;
    return s;
}

int probe_agents_count(void) { return s_n; }
const probe_agent_t *probe_agents_at(int i) { return (i >= 0 && i < s_n) ? &s_agents[i] : 0; }

int probe_agents_lifecycle(uint32_t now_ms)
{
    int reborn = 0;
    for (int i = 0; i < s_n; i++) {
        probe_agent_t *a = &s_agents[i];
        if (a->alive && (now_ms - a->born_ms) >= a->life_ms) {
            agent_spawn(a, now_ms);      // dies, then reincarnates with a fresh random identity
            reborn++;
        }
    }
    return reborn;
}

static uint32_t next_interval(const probe_agent_t *a)
{
    return (a->duty == DUTY_ACTIVE) ? rnd_range(ACTIVE_MIN_MS, ACTIVE_MAX_MS)
                                    : rnd_range(IDLE_MIN_MS, IDLE_MAX_MS);
}

int probe_agents_due(uint32_t now_ms, probe_agent_t **out, int max)
{
    int k = 0;
    for (int i = 0; i < s_n && k < max; i++) {
        probe_agent_t *a = &s_agents[i];
        if (a->alive && (int32_t)(now_ms - a->next_scan_ms) >= 0) {
            out[k++] = a;
            a->next_scan_ms = now_ms + next_interval(a);   // reschedule with jittered per-duty interval
        }
    }
    return k;
}
