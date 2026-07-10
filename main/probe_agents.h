#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "probe_frame.h"

// Independent fake-phone "agents". Each owns its identity (MAC + archetype), its own 802.11
// sequence counter, a jittered scan schedule, and a bounded lifetime. The population turns over
// (birth/death) so the set never stabilizes into a constellation fingerprint. Pure/host-testable:
// no ESP radio/timer calls; the clock arrives as now_ms, randomness via esp_random().
#define PROBE_AGENTS_MAX 16

typedef enum { DUTY_ACTIVE, DUTY_IDLE } probe_duty_t;

typedef struct {
    uint8_t      mac[6];
    probe_arch_t arch;          // bound to the MAC for the agent's whole life
    uint16_t     seq;           // 12-bit, monotonic per agent
    probe_duty_t duty;
    uint32_t     next_scan_ms;
    uint32_t     born_ms;
    uint32_t     life_ms;       // bounded lifetime; on expiry the agent dies + reincarnates
    bool         alive;
} probe_agent_t;

void     probe_agents_init(int n, uint32_t now_ms);          // (re)seed n agents (<= PROBE_AGENTS_MAX)
int      probe_agents_lifecycle(uint32_t now_ms);            // retire+reincarnate expired; returns #reborn
int      probe_agents_due(uint32_t now_ms, probe_agent_t **out, int max);  // due subset; reschedules them
uint16_t probe_agent_next_seq(probe_agent_t *a);             // return current seq, then +1 (12-bit wrap)
int      probe_agents_count(void);
const probe_agent_t *probe_agents_at(int i);
