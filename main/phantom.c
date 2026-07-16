#include "phantom.h"
#include "esp_random.h"
#include "probe_agents.h"

// Phone-like lifetime band: a persona is a person's phone passing through or lingering.
#define PHANTOM_LIFE_MIN_MS   180000u    // 3 min
#define PHANTOM_LIFE_MAX_MS  2400000u    // 40 min
// Realistic phone-family mix (weights). Apple leads; matches a phone-heavy environment.
static const uint8_t FAMILY_W[PHANTOM_FAMILY_COUNT] = { 25, 12, 45, 18 }; // Samsung,Google,Apple,Generic

static phantom_t s_ph[PHANTOM_MAX];
static int       s_n;

static uint32_t rnd_range(uint32_t lo, uint32_t hi) { return lo + (esp_random() % (hi - lo + 1u)); }

static phantom_family_t pick_family(void) {
    uint32_t total = 0;
    for (int i = 0; i < PHANTOM_FAMILY_COUNT; i++) total += FAMILY_W[i];
    uint32_t r = esp_random() % total;
    for (int i = 0; i < PHANTOM_FAMILY_COUNT; i++) {
        if (r < FAMILY_W[i]) return (phantom_family_t)i;
        r -= FAMILY_W[i];
    }
    return PF_GENERIC;
}

static void ph_spawn(phantom_t *ph, uint32_t now_ms) {
    ph->family     = pick_family();
    ph->born_ms    = now_ms;
    ph->life_ms    = rnd_range(PHANTOM_LIFE_MIN_MS, PHANTOM_LIFE_MAX_MS);
    ph->generation = ph->generation + 1u;   // starts at 1 on first spawn (struct zero-inited)
    ph->alive      = true;
}

void phantom_init(int n, uint32_t now_ms) {
    if (n > PHANTOM_MAX) n = PHANTOM_MAX;
    if (n < 1) n = 1;
    s_n = n;
    for (int i = 0; i < s_n; i++) { s_ph[i].generation = 0; ph_spawn(&s_ph[i], now_ms); }
}

int phantom_lifecycle(uint32_t now_ms) {
    int reborn = 0;
    for (int i = 0; i < s_n; i++) {
        phantom_t *ph = &s_ph[i];
        if (ph->alive && (now_ms - ph->born_ms) >= ph->life_ms) { ph_spawn(ph, now_ms); reborn++; }
    }
    return reborn;
}

int phantom_count(void) { return s_n; }
const phantom_t *phantom_at(int i) { return (i >= 0 && i < s_n) ? &s_ph[i] : 0; }

probe_arch_t phantom_arch(phantom_family_t f) {
    switch (f) {
        case PF_SAMSUNG: return ARCH_GALAXY;
        case PF_GOOGLE:  return ARCH_PIXEL;
        case PF_APPLE:   return ARCH_IPHONE;
        default:         return ARCH_ANDROID;   // PF_GENERIC
    }
}

uint16_t phantom_company(phantom_family_t f) {
    switch (f) {
        case PF_SAMSUNG: return 0x0075;   // Samsung
        case PF_GOOGLE:  return 0x00E0;   // Google
        default:         return 0;        // Apple/generic -> anonymous RPA (Law-3-safe)
    }
}

void phantom_sync_wifi(uint32_t now_ms)
{
    (void)now_ms;
    for (int i = 0; i < s_n; i++) {
        const phantom_t *ph = &s_ph[i];
        probe_agent_sync(i, phantom_arch(ph->family), ph->born_ms, ph->life_ms, ph->generation);
    }
}
