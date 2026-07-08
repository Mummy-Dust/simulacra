# Vigil Runtime Settings + Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let Vigil (CYD) push behaviour presets to every decoy over ESP-NOW, backed by a runtime settings backend (RAM+NVS) shared with the web UI, authenticated with an Ed25519 signature and bounded by a safe-value clamp.

**Architecture:** A decoy-side `settings` module is the single source of truth for churn tunables; it resolves a preset to concrete values against the board's own ceiling, clamps to safe floors, applies to the churn engine, and persists to NVS. A new AES-GCM-sealed `CONFIG` frame (type 7) carries a signed `config_cmd_t`; the Ed25519 signature over `nonce‖cmd` is verified with Vigil's public key (decoys never hold the private key). Vigil gains a touch CONTROL page that selects a preset and broadcasts it.

**Tech Stack:** ESP-IDF v5.5 (C5) / v5.4 (CYD, C6), C11, NimBLE, ESP-NOW, NVS, mbedTLS AES-GCM (existing `radar_wire`), vendored TweetNaCl (Ed25519), XPT2046 (bit-banged SPI), on-target `ST_CHECK` selftest.

## Global Constraints

- Law 3 (no raw identifiers off-device) is unaffected: CONFIG carries only a preset id; STATUS is unchanged.
- Frame type ids in use: 1 REQUEST, 2 STATUS, 3 LEARN_OFFER, 4 LEARN_SYNC, 5 SIG_SYNC, 6 FLEET_MACS. **CONFIG = 7.**
- All new persona-split / component files that read `CONFIG_*` macros must `#include "sdkconfig.h"` first (see [[esp-idf-v55-sdkconfig-include]]).
- `-D<FLAG>=<val>` reaches the compiler only if the flag is added to the `foreach` block in `main/CMakeLists.txt` (decoy) or `cyd/main/CMakeLists.txt` (Vigil). New gate: `SIMULACRA_CONFIG_CTRL`.
- Selftest builds with `-DCHURN_SELFTEST=1`; pass `-DCHURN_SELFTEST=0` explicitly when rebuilding a decoy/Vigil image afterward (CMakeCache persists `-D` values).
- AI trailers stay in commits (policy 2026-07-07). Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- Preset ladder (semantic; each decoy clamps to its own `CHURN_ACTIVE_SET`): PAUSE / STEALTH / NORMAL / DENSE / MAX.
- Clamp floors (a forged/defeated command cannot cross these): `active_target ≥ 4`, `dwell_min ≥ 30000ms`, `dwell_max ≤ 900000ms`, `cooldown_min ≥ 300000ms`, `accel ∈ [1.0, 4.0]`. PAUSE only freezes rotation (phantoms stay on-air); detection toggle is never exposed over CONFIG.

---

### Task 1: Runtime dwell/cooldown knobs in the churn engine

Promote the compile-time dwell/cooldown `#define`s to runtime RAM state with setters, so the settings backend can drive them. `active_target`/`paused`/`accel` are already runtime.

**Files:**
- Modify: `main/churn.h` (add declarations)
- Modify: `main/churn.c:12-44,63-80` (add state, setters, use vars)
- Test: `main/churn_selftest.c` (new `test_churn_runtime_knobs`)

**Interfaces:**
- Produces:
  - `void churn_set_dwell_ms(uint32_t lo, uint32_t hi);`
  - `void churn_set_cooldown_ms(uint32_t lo, uint32_t hi);`
  - `void churn_get_dwell_ms(uint32_t *lo, uint32_t *hi);`
  - `void churn_get_cooldown_ms(uint32_t *lo, uint32_t *hi);`
- Consumes: existing `CHURN_DWELL_MIN_MS/MAX_MS`, `CHURN_COOLDOWN_MIN_MS/MAX_MS` as firmware defaults.

- [ ] **Step 1: Write the failing test**

Add to `main/churn_selftest.c` (before `churn_selftest_run`):

```c
static void test_churn_runtime_knobs(void)
{
    uint32_t lo = 0, hi = 0;
    churn_set_dwell_ms(50000, 60000);
    churn_get_dwell_ms(&lo, &hi);
    ST_CHECK(lo == 50000 && hi == 60000, "dwell setter/getter round-trip");
    churn_set_cooldown_ms(400000, 500000);
    churn_get_cooldown_ms(&lo, &hi);
    ST_CHECK(lo == 400000 && hi == 500000, "cooldown setter/getter round-trip");
    // Restore firmware defaults so later tests are unaffected.
    churn_set_dwell_ms(CHURN_DWELL_MIN_MS, CHURN_DWELL_MAX_MS);
    churn_set_cooldown_ms(CHURN_COOLDOWN_MIN_MS, CHURN_COOLDOWN_MAX_MS);
}
```

Register it in `churn_selftest_run` next to the other `test_churn_*` calls:

```c
    test_churn_runtime_knobs();
```

- [ ] **Step 2: Run selftest build, verify it fails to compile (undefined setters)**

Run: `idf.py -DCHURN_SELFTEST=1 build` (from `~/Downloads/splinter`, C5/IDF5.5 env)
Expected: FAIL — `implicit declaration of function 'churn_set_dwell_ms'`.

- [ ] **Step 3: Add declarations to `main/churn.h`**

After the existing `churn_set_accel` declaration (around line 35):

```c
// Runtime dwell/cooldown windows (ms). Default to the CHURN_DWELL_*/CHURN_COOLDOWN_* firmware
// constants; the settings backend overrides them per preset. lo<=hi enforced by the setter.
void churn_set_dwell_ms(uint32_t lo, uint32_t hi);
void churn_set_cooldown_ms(uint32_t lo, uint32_t hi);
void churn_get_dwell_ms(uint32_t *lo, uint32_t *hi);
void churn_get_cooldown_ms(uint32_t *lo, uint32_t *hi);
```

- [ ] **Step 4: Add state + setters + swap macro uses in `main/churn.c`**

Add statics after line 19 (`static float s_accel...`):

```c
static uint32_t s_dwell_lo = CHURN_DWELL_MIN_MS, s_dwell_hi = CHURN_DWELL_MAX_MS;
static uint32_t s_cool_lo  = CHURN_COOLDOWN_MIN_MS, s_cool_hi = CHURN_COOLDOWN_MAX_MS;
```

Add setters/getters after `churn_set_accel` (line 37):

```c
void churn_set_dwell_ms(uint32_t lo, uint32_t hi)
{ if (hi < lo) hi = lo; s_dwell_lo = lo; s_dwell_hi = hi; }
void churn_set_cooldown_ms(uint32_t lo, uint32_t hi)
{ if (hi < lo) hi = lo; s_cool_lo = lo; s_cool_hi = hi; }
void churn_get_dwell_ms(uint32_t *lo, uint32_t *hi) { if (lo) *lo = s_dwell_lo; if (hi) *hi = s_dwell_hi; }
void churn_get_cooldown_ms(uint32_t *lo, uint32_t *hi) { if (lo) *lo = s_cool_lo; if (hi) *hi = s_cool_hi; }
```

In `dwell_ms()` (line 41) replace the macro range:

```c
    uint32_t d = rnd_range(s_dwell_lo, s_dwell_hi);
```

In `churn_init` staggered seed (line 58) replace `CHURN_DWELL_MAX_MS`:

```c
            id->active_until_ms = now_ms + rnd_range(1, s_dwell_hi); // staggered seed
```

In `churn_tick` cooldown assignment (line 70) replace the macro range:

```c
            id->eligible_at_ms = now_ms + rnd_range(s_cool_lo, s_cool_hi);
```

- [ ] **Step 5: Rebuild selftest, verify PASS**

Run: `idf.py -DCHURN_SELFTEST=1 build flash monitor` (C5)
Expected: serial `SELFTEST: PASS (n/n)` with n increased by 2.

- [ ] **Step 6: Commit**

```bash
git add main/churn.h main/churn.c main/churn_selftest.c
git commit -m "feat(churn): runtime dwell/cooldown knobs (backing the settings backend)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Settings backend — pure preset resolve + clamp

The pure core: map a preset to concrete settings against a board ceiling, and clamp any settings to safe floors. No churn/NVS side effects here, so it is fully testable.

**Files:**
- Create: `main/settings.h`
- Create: `main/settings.c` (resolve + clamp only in this task)
- Modify: `main/CMakeLists.txt` (add `settings.c` to `SRCS` if not glob)
- Test: `main/churn_selftest.c` (new `test_settings_resolve`)

**Interfaces:**
- Produces:
  - `typedef enum { SIM_PRESET_PAUSE=0, SIM_PRESET_STEALTH, SIM_PRESET_NORMAL, SIM_PRESET_DENSE, SIM_PRESET_MAX, SIM_PRESET_COUNT } sim_preset_t;`
  - `typedef struct { uint8_t active_target; bool paused; float accel; uint32_t dwell_min_ms, dwell_max_ms, cooldown_min_ms, cooldown_max_ms; } sim_settings_t;`
  - `int  sim_settings_resolve(sim_preset_t p, uint8_t ceiling, sim_settings_t *out);` (0 ok, -1 bad preset)
  - `void sim_settings_clamp(sim_settings_t *s, uint8_t ceiling);`
  - `#define SIM_TARGET_FLOOR 4`

- [ ] **Step 1: Write the failing test**

Add to `main/churn_selftest.c` (`#include "settings.h"` near the top with the other includes):

```c
static void test_settings_resolve(void)
{
    sim_settings_t s;
    ST_CHECK(sim_settings_resolve(SIM_PRESET_NORMAL, 16, &s) == 0, "resolve NORMAL ok");
    ST_CHECK(s.active_target == 16 && !s.paused && s.accel == 1.0f, "NORMAL fills ceiling, running");

    ST_CHECK(sim_settings_resolve(SIM_PRESET_STEALTH, 16, &s) == 0, "resolve STEALTH ok");
    ST_CHECK(s.active_target == 6 && s.dwell_min_ms >= 300000, "STEALTH ~40% ceiling, long dwell");

    ST_CHECK(sim_settings_resolve(SIM_PRESET_MAX, 16, &s) == 0, "resolve MAX ok");
    ST_CHECK(s.active_target == 16 && s.accel > 2.0f && s.dwell_max_ms <= 120000, "MAX cranks turnover");

    ST_CHECK(sim_settings_resolve(SIM_PRESET_PAUSE, 16, &s) == 0, "resolve PAUSE ok");
    ST_CHECK(s.paused && s.active_target == 16, "PAUSE freezes rotation, crowd stays on-air");

    ST_CHECK(sim_settings_resolve(SIM_PRESET_COUNT, 16, &s) == -1, "bad preset rejected");

    // Clamp floors: a hostile 'target=0, dwell=0' can't cross safe bounds.
    sim_settings_t bad = { .active_target = 0, .paused = false, .accel = 9.0f,
                           .dwell_min_ms = 0, .dwell_max_ms = 5, .cooldown_min_ms = 0, .cooldown_max_ms = 0 };
    sim_settings_clamp(&bad, 16);
    ST_CHECK(bad.active_target >= SIM_TARGET_FLOOR, "clamp raises target to floor");
    ST_CHECK(bad.dwell_min_ms >= 30000 && bad.accel <= 4.0f && bad.cooldown_min_ms >= 300000, "clamp bounds dwell/accel/cooldown");

    // Ceiling honored on a smaller board (Shade-like ceiling).
    ST_CHECK(sim_settings_resolve(SIM_PRESET_MAX, 8, &s) == 0 && s.active_target == 8, "MAX clamps to board ceiling");
}
```

Register in `churn_selftest_run`:

```c
    test_settings_resolve();
```

- [ ] **Step 2: Run selftest build, verify it fails (no settings.h)**

Run: `idf.py -DCHURN_SELFTEST=1 build`
Expected: FAIL — `settings.h: No such file or directory`.

- [ ] **Step 3: Create `main/settings.h`**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SIM_TARGET_FLOOR 4   // clamp floor: crowd never shrinks below this (STEALTH density)

typedef enum {
    SIM_PRESET_PAUSE = 0, SIM_PRESET_STEALTH, SIM_PRESET_NORMAL,
    SIM_PRESET_DENSE, SIM_PRESET_MAX, SIM_PRESET_COUNT
} sim_preset_t;

typedef struct {
    uint8_t  active_target;                       // concurrent phantom crowd size
    bool     paused;                              // freeze rotation (phantoms stay on-air)
    float    accel;                               // dwell shortening multiplier (>=1.0)
    uint32_t dwell_min_ms, dwell_max_ms;          // per-identity on-air window
    uint32_t cooldown_min_ms, cooldown_max_ms;    // retirement cooldown window
} sim_settings_t;

// Pure: resolve preset p to concrete settings against this board's `ceiling` (CHURN_ACTIVE_SET),
// already clamped. Returns 0 on success, -1 for an unknown preset. No side effects.
int  sim_settings_resolve(sim_preset_t p, uint8_t ceiling, sim_settings_t *out);

// Clamp settings to safe floors/ceilings in place (idempotent). Used on every apply so a forged
// or malformed command can never cross safe bounds.
void sim_settings_clamp(sim_settings_t *s, uint8_t ceiling);
```

- [ ] **Step 4: Create `main/settings.c` (resolve + clamp)**

```c
#include "settings.h"
#include "churn.h"    // CHURN_DWELL_*/CHURN_COOLDOWN_* firmware defaults

static uint32_t u32_clamp(uint32_t v, uint32_t lo, uint32_t hi)
{ return v < lo ? lo : (v > hi ? hi : v); }

void sim_settings_clamp(sim_settings_t *s, uint8_t ceiling)
{
    if (ceiling < SIM_TARGET_FLOOR) ceiling = SIM_TARGET_FLOOR;
    if (s->active_target < SIM_TARGET_FLOOR) s->active_target = SIM_TARGET_FLOOR;
    if (s->active_target > ceiling) s->active_target = ceiling;
    if (s->accel < 1.0f) s->accel = 1.0f;
    if (s->accel > 4.0f) s->accel = 4.0f;
    s->dwell_min_ms = u32_clamp(s->dwell_min_ms, 30000u, 900000u);
    s->dwell_max_ms = u32_clamp(s->dwell_max_ms, s->dwell_min_ms, 900000u);
    s->cooldown_min_ms = u32_clamp(s->cooldown_min_ms, 300000u, 3600000u);
    s->cooldown_max_ms = u32_clamp(s->cooldown_max_ms, s->cooldown_min_ms, 3600000u);
}

int sim_settings_resolve(sim_preset_t p, uint8_t ceiling, sim_settings_t *out)
{
    if (p >= SIM_PRESET_COUNT) return -1;
    uint8_t stealth = (uint8_t)((ceiling * 4) / 10);   // ~40% of ceiling
    sim_settings_t s = {
        .active_target = ceiling, .paused = false, .accel = 1.0f,
        .dwell_min_ms = CHURN_DWELL_MIN_MS, .dwell_max_ms = CHURN_DWELL_MAX_MS,
        .cooldown_min_ms = CHURN_COOLDOWN_MIN_MS, .cooldown_max_ms = CHURN_COOLDOWN_MAX_MS,
    };
    switch (p) {
    case SIM_PRESET_PAUSE:                                  // NORMAL values, rotation frozen
        s.paused = true; break;
    case SIM_PRESET_STEALTH:
        s.active_target = stealth; s.dwell_min_ms = 300000; s.dwell_max_ms = 600000; break;
    case SIM_PRESET_NORMAL:
        break;                                              // firmware defaults
    case SIM_PRESET_DENSE:
        s.accel = 1.5f; s.dwell_min_ms = 90000; s.dwell_max_ms = 240000;
        s.cooldown_min_ms = 900000; s.cooldown_max_ms = 1800000; break;
    case SIM_PRESET_MAX:
        s.accel = 2.5f; s.dwell_min_ms = 45000; s.dwell_max_ms = 120000;
        s.cooldown_min_ms = 600000; s.cooldown_max_ms = 1200000; break;
    default: return -1;
    }
    sim_settings_clamp(&s, ceiling);
    *out = s;
    return 0;
}
```

- [ ] **Step 5: Ensure `settings.c` is built**

Check `main/CMakeLists.txt`: if `SRCS` is an explicit list, add `"settings.c"`. If it globs `*.c`, no change. Confirm with a build.

- [ ] **Step 6: Rebuild selftest, verify PASS**

Run: `idf.py -DCHURN_SELFTEST=1 build flash monitor` (C5)
Expected: `SELFTEST: PASS (n/n)`.

- [ ] **Step 7: Commit**

```bash
git add main/settings.h main/settings.c main/churn_selftest.c main/CMakeLists.txt
git commit -m "feat(settings): pure preset resolve + safe-bounds clamp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Settings backend — apply, init, NVS persistence, granular set/get

Wire the pure core to the churn engine and NVS. Boot loads persisted settings (or firmware defaults); presets and web-UI writes go through here.

**Files:**
- Modify: `main/settings.h` (add apply/init/get/set/apply_preset)
- Modify: `main/settings.c` (implement; add NVS)
- Modify: `main/simulacra_main.c` (call `sim_settings_init()` after `churn_init`)
- Test: `main/churn_selftest.c` (new `test_settings_apply`)

**Interfaces:**
- Consumes: Task 1 churn setters; Task 2 resolve/clamp; `CHURN_ACTIVE_SET`.
- Produces:
  - `void sim_settings_apply(const sim_settings_t *s);`  (calls churn setters; no persist)
  - `int  sim_settings_apply_preset(sim_preset_t p);`    (resolve→clamp→apply→persist; 0 ok, -1 bad)
  - `void sim_settings_init(void);`                       (load NVS or defaults, then apply)
  - `void sim_settings_get(sim_settings_t *out);`         (current in-RAM settings)
  - `void sim_settings_set(const sim_settings_t *s);`     (clamp→apply→persist; web-UI granular path)

- [ ] **Step 1: Write the failing test**

Add to `main/churn_selftest.c`:

```c
static void test_settings_apply(void)
{
    roster_init(); churn_set_apply(noop_apply);
    sim_settings_apply_preset(SIM_PRESET_STEALTH);
    ST_CHECK(churn_active_target() == (uint8_t)((CHURN_ACTIVE_SET*4)/10), "apply STEALTH sets churn target");
    ST_CHECK(!churn_paused(), "STEALTH is running");
    uint32_t lo=0,hi=0; churn_get_dwell_ms(&lo,&hi);
    ST_CHECK(lo == 300000, "apply STEALTH sets churn dwell");

    sim_settings_apply_preset(SIM_PRESET_PAUSE);
    ST_CHECK(churn_paused(), "apply PAUSE pauses churn");

    ST_CHECK(sim_settings_apply_preset(SIM_PRESET_COUNT) == -1, "apply bad preset rejected");

    sim_settings_t g; sim_settings_get(&g);
    ST_CHECK(g.paused, "get reflects last applied (PAUSE)");

    // Restore NORMAL so subsequent tests run with defaults.
    sim_settings_apply_preset(SIM_PRESET_NORMAL);
    churn_set_dwell_ms(CHURN_DWELL_MIN_MS, CHURN_DWELL_MAX_MS);
    churn_set_cooldown_ms(CHURN_COOLDOWN_MIN_MS, CHURN_COOLDOWN_MAX_MS);
}
```

Register in `churn_selftest_run`: `test_settings_apply();`

- [ ] **Step 2: Run selftest, verify fail (undefined apply funcs)**

Run: `idf.py -DCHURN_SELFTEST=1 build`
Expected: FAIL — undefined `sim_settings_apply_preset`.

- [ ] **Step 3: Extend `main/settings.h`**

Append:

```c
// Apply settings to the churn engine now (no persistence).
void sim_settings_apply(const sim_settings_t *s);
// Resolve preset against CHURN_ACTIVE_SET, clamp, apply, and persist. 0 ok, -1 unknown preset.
int  sim_settings_apply_preset(sim_preset_t p);
// Load persisted settings from NVS (or firmware defaults) and apply. Call once at boot.
void sim_settings_init(void);
// Snapshot the current in-RAM settings.
void sim_settings_get(sim_settings_t *out);
// Web-UI granular path: clamp, apply, and persist an explicit settings struct.
void sim_settings_set(const sim_settings_t *s);
```

- [ ] **Step 4: Implement in `main/settings.c`**

Add includes + state + functions:

```c
#include "churn_adv.h"        // CHURN_HW_INSTANCES (via churn.h already); harmless if present
#include "nvs.h"
#include <string.h>

#define SETTINGS_NVS_NS  "sim"
#define SETTINGS_NVS_KEY "settings"

static sim_settings_t s_cur;   // current in-RAM settings (source of truth)

void sim_settings_apply(const sim_settings_t *s)
{
    churn_set_active_target(s->active_target);
    churn_set_paused(s->paused);
    churn_set_accel(s->accel);
    churn_set_dwell_ms(s->dwell_min_ms, s->dwell_max_ms);
    churn_set_cooldown_ms(s->cooldown_min_ms, s->cooldown_max_ms);
    s_cur = *s;
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;   // best-effort
    nvs_set_blob(h, SETTINGS_NVS_KEY, &s_cur, sizeof s_cur);
    nvs_commit(h); nvs_close(h);
}

void sim_settings_set(const sim_settings_t *s)
{
    sim_settings_t c = *s; sim_settings_clamp(&c, CHURN_ACTIVE_SET);
    sim_settings_apply(&c); settings_save();
}

int sim_settings_apply_preset(sim_preset_t p)
{
    sim_settings_t s;
    if (sim_settings_resolve(p, CHURN_ACTIVE_SET, &s) != 0) return -1;
    sim_settings_apply(&s); settings_save();
    return 0;
}

void sim_settings_get(sim_settings_t *out) { *out = s_cur; }

void sim_settings_init(void)
{
    sim_settings_t s;
    nvs_handle_t h; size_t len = sizeof s;
    bool loaded = (nvs_open(SETTINGS_NVS_NS, NVS_READONLY, &h) == ESP_OK) &&
                  (nvs_get_blob(h, SETTINGS_NVS_KEY, &s, &len) == ESP_OK) && len == sizeof s;
    if (loaded) nvs_close(h);
    if (!loaded) sim_settings_resolve(SIM_PRESET_NORMAL, CHURN_ACTIVE_SET, &s);
    sim_settings_clamp(&s, CHURN_ACTIVE_SET);   // guard against a stale/foreign blob
    sim_settings_apply(&s);
}
```

- [ ] **Step 5: Call `sim_settings_init()` at boot**

In `main/simulacra_main.c`, add `#include "settings.h"` and, immediately after the existing `churn_init(...)` call, add:

```c
    sim_settings_init();   // restore persisted churn tunables (or firmware defaults)
```

- [ ] **Step 6: Rebuild selftest, verify PASS**

Run: `idf.py -DCHURN_SELFTEST=1 build flash monitor` (C5)
Expected: `SELFTEST: PASS (n/n)`.

- [ ] **Step 7: On-target persistence check (manual)**

Build a normal decoy image (`idf.py -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1 build flash monitor`). Confirm boot log shows churn running; this task's persistence is exercised end-to-end in Task 7.

- [ ] **Step 8: Commit**

```bash
git add main/settings.h main/settings.c main/simulacra_main.c main/churn_selftest.c
git commit -m "feat(settings): apply-to-churn, NVS persistence, boot restore, granular set/get

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Vendor TweetNaCl (Ed25519) as a shared component

ESP-IDF's mbedTLS has no Ed25519 signatures. Vendor TweetNaCl (public domain, single file) into a component usable by both `main/` (decoy verify) and `cyd/` (Vigil sign). Signing is deterministic — no on-device RNG for sign/verify; a `randombytes` stub satisfies the linker (only keygen, done offline, would use it).

**Files:**
- Create: `components/tweetnacl/tweetnacl.c` (upstream, unmodified)
- Create: `components/tweetnacl/tweetnacl.h` (upstream)
- Create: `components/tweetnacl/randombytes.c` (esp_fill_random-backed stub)
- Create: `components/tweetnacl/CMakeLists.txt`

**Interfaces:**
- Produces (from upstream `tweetnacl.h`):
  - `int crypto_sign(unsigned char *sm, unsigned long long *smlen, const unsigned char *m, unsigned long long n, const unsigned char *sk);`
  - `int crypto_sign_open(unsigned char *m, unsigned long long *mlen, const unsigned char *sm, unsigned long long n, const unsigned char *pk);`
  - `#define crypto_sign_PUBLICKEYBYTES 32`, `crypto_sign_SECRETKEYBYTES 64`, `crypto_sign_BYTES 64`.

- [ ] **Step 1: Fetch upstream TweetNaCl**

Download the canonical files (TweetNaCl v20140427, public domain) into `components/tweetnacl/`:
- `tweetnacl.c` and `tweetnacl.h` from https://tweetnacl.cr.yp.to/ (or the widely-mirrored `github.com/dominictarr/tweetnacl` C sources). Do **not** edit them.

Verify the header exposes the `crypto_sign*` macros/prototypes above.

- [ ] **Step 2: Create `components/tweetnacl/randombytes.c`**

```c
// TweetNaCl references randombytes() (only crypto_*_keypair use it; we generate keys offline).
// Provide an esp_fill_random-backed definition so the symbol resolves on-device.
#include <stdint.h>
#include "esp_random.h"
void randombytes(unsigned char *x, unsigned long long xlen)
{
    while (xlen > 0) {
        uint32_t chunk = (xlen > 4) ? 4 : (uint32_t)xlen;
        uint32_t r = esp_random();
        for (uint32_t i = 0; i < chunk; i++) { x[i] = (unsigned char)(r & 0xFF); r >>= 8; }
        x += chunk; xlen -= chunk;
    }
}
```

- [ ] **Step 3: Create `components/tweetnacl/CMakeLists.txt`**

```cmake
idf_component_register(SRCS "tweetnacl.c" "randombytes.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_hw_support)
```

- [ ] **Step 4: Make `simulacra_radar` depend on tweetnacl**

In `components/simulacra_radar/CMakeLists.txt`, add `tweetnacl` to `REQUIRES` (config_wire in Task 5 uses it). Confirm the exact key name (`REQUIRES` vs `PRIV_REQUIRES`) matches the existing file style.

- [ ] **Step 5: Build both projects, verify link**

Run: `idf.py -DCHURN_SELFTEST=1 build` (decoy, C5) and `cd cyd && idf.py build` (CYD).
Expected: both link with no `undefined reference to randombytes/crypto_sign`.

- [ ] **Step 6: Commit**

```bash
git add components/tweetnacl/
git commit -m "chore(crypto): vendor TweetNaCl (Ed25519) + randombytes stub

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: CONFIG wire format — signed pack/open

The `CONFIG` frame payload: `cmd(2) ‖ sig(64)`, where sig is Ed25519 over `nonce(12) ‖ cmd(2)`. This binds the signature to the exact sealed frame so a PSK holder cannot re-seal a captured signed command under a new nonce.

**Files:**
- Create: `components/simulacra_radar/config_wire.h`
- Create: `components/simulacra_radar/config_wire.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `config_wire.c` if explicit SRCS)
- Test: `main/churn_selftest.c` (new `test_config_wire`)

**Interfaces:**
- Consumes: Task 4 `crypto_sign`/`crypto_sign_open`; Task 2 `sim_preset_t` is NOT needed here (preset id is a raw `uint8_t`).
- Produces:
  - `#define RADAR_TYPE_CONFIG 7`, `#define CONFIG_WIRE_VER 1`, `#define CONFIG_SIG_LEN 64`
  - `typedef struct __attribute__((packed)) { uint8_t version; uint8_t preset_id; } config_cmd_t;`
  - `#define CONFIG_WIRE_PAYLOAD_LEN (sizeof(config_cmd_t) + CONFIG_SIG_LEN)`  (66)
  - `int config_wire_pack_signed(uint8_t *out, size_t out_cap, const config_cmd_t *cmd, const uint8_t nonce12[12], const uint8_t sk[64]);` (returns payload len, or -1)
  - `int config_wire_open_signed(const uint8_t *pl, size_t pl_len, const uint8_t nonce12[12], const uint8_t pk[32], config_cmd_t *cmd_out);` (0 ok, -1 fail)

- [ ] **Step 1: Write the failing test**

Add to `main/churn_selftest.c` (`#include "config_wire.h"`; the test uses a fixed keypair generated in Task 6 — for this task, generate a throwaway keypair on-device with `crypto_sign_keypair`):

```c
#include "tweetnacl.h"
static void test_config_wire(void)
{
    uint8_t pk[32], sk[64];
    crypto_sign_keypair(pk, sk);                       // ephemeral test keypair
    uint8_t nonce[12]; for (int i=0;i<12;i++) nonce[i] = (uint8_t)(i*7+1);
    config_cmd_t cmd = { .version = CONFIG_WIRE_VER, .preset_id = 3 };

    uint8_t pl[CONFIG_WIRE_PAYLOAD_LEN];
    int n = config_wire_pack_signed(pl, sizeof pl, &cmd, nonce, sk);
    ST_CHECK(n == CONFIG_WIRE_PAYLOAD_LEN, "pack returns payload len");

    config_cmd_t got;
    ST_CHECK(config_wire_open_signed(pl, n, nonce, pk, &got) == 0, "open verifies good sig");
    ST_CHECK(got.preset_id == 3 && got.version == CONFIG_WIRE_VER, "open recovers cmd");

    pl[0] ^= 0x01;                                      // tamper cmd byte
    ST_CHECK(config_wire_open_signed(pl, n, nonce, pk, &got) != 0, "tampered cmd fails verify");
    pl[0] ^= 0x01;                                      // restore

    uint8_t nonce2[12]; memcpy(nonce2, nonce, 12); nonce2[0] ^= 0x01;
    ST_CHECK(config_wire_open_signed(pl, n, nonce2, pk, &got) != 0, "nonce mismatch fails verify");
}
```

Register in `churn_selftest_run`: `test_config_wire();`

- [ ] **Step 2: Run selftest, verify fail (no config_wire.h)**

Run: `idf.py -DCHURN_SELFTEST=1 build`
Expected: FAIL — `config_wire.h: No such file or directory`.

- [ ] **Step 3: Create `components/simulacra_radar/config_wire.h`**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>

#define RADAR_TYPE_CONFIG 7          // Vigil -> all decoys: signed settings preset
#define CONFIG_WIRE_VER   1
#define CONFIG_SIG_LEN    64

typedef struct __attribute__((packed)) {
    uint8_t version;                 // CONFIG_WIRE_VER
    uint8_t preset_id;               // sim_preset_t value (validated on the decoy)
} config_cmd_t;

#define CONFIG_WIRE_PAYLOAD_LEN (sizeof(config_cmd_t) + CONFIG_SIG_LEN)   // 66

// Vigil: build payload = cmd || Ed25519_sig(nonce12 || cmd) with secret key sk[64].
// nonce12 = salt(4) || counter(8 BE) — the SAME nonce radar_wire_seal will use.
// Returns the payload length, or -1 on buffer/sign error.
int config_wire_pack_signed(uint8_t *out, size_t out_cap, const config_cmd_t *cmd,
                            const uint8_t nonce12[12], const uint8_t sk[64]);

// Decoy: verify payload against (nonce12 || cmd) with public key pk[32].
// Returns 0 and fills *cmd_out on a valid signature; -1 on any failure.
int config_wire_open_signed(const uint8_t *pl, size_t pl_len, const uint8_t nonce12[12],
                            const uint8_t pk[32], config_cmd_t *cmd_out);
```

- [ ] **Step 4: Create `components/simulacra_radar/config_wire.c`**

```c
#include "config_wire.h"
#include "tweetnacl.h"
#include <string.h>

// Signed message m = nonce12(12) || cmd(2)  (14 bytes)
#define CW_MSG_LEN (12 + sizeof(config_cmd_t))

int config_wire_pack_signed(uint8_t *out, size_t out_cap, const config_cmd_t *cmd,
                            const uint8_t nonce12[12], const uint8_t sk[64])
{
    if (out_cap < CONFIG_WIRE_PAYLOAD_LEN) return -1;
    uint8_t m[CW_MSG_LEN];
    memcpy(m, nonce12, 12);
    memcpy(m + 12, cmd, sizeof *cmd);
    uint8_t sm[CONFIG_SIG_LEN + CW_MSG_LEN];            // crypto_sign output = sig || m
    unsigned long long smlen = 0;
    if (crypto_sign(sm, &smlen, m, CW_MSG_LEN, sk) != 0) return -1;
    memcpy(out, cmd, sizeof *cmd);                      // payload = cmd || sig
    memcpy(out + sizeof *cmd, sm, CONFIG_SIG_LEN);      // sig is the first 64 bytes of sm
    return (int)CONFIG_WIRE_PAYLOAD_LEN;
}

int config_wire_open_signed(const uint8_t *pl, size_t pl_len, const uint8_t nonce12[12],
                            const uint8_t pk[32], config_cmd_t *cmd_out)
{
    if (pl_len != CONFIG_WIRE_PAYLOAD_LEN) return -1;
    config_cmd_t cmd; memcpy(&cmd, pl, sizeof cmd);
    uint8_t m[CW_MSG_LEN];
    memcpy(m, nonce12, 12);
    memcpy(m + 12, &cmd, sizeof cmd);
    uint8_t sm[CONFIG_SIG_LEN + CW_MSG_LEN];            // reconstruct sig || m
    memcpy(sm, pl + sizeof cmd, CONFIG_SIG_LEN);
    memcpy(sm + CONFIG_SIG_LEN, m, CW_MSG_LEN);
    uint8_t out[CONFIG_SIG_LEN + CW_MSG_LEN];           // crypto_sign_open writes the recovered m
    unsigned long long outlen = 0;
    if (crypto_sign_open(out, &outlen, sm, CONFIG_SIG_LEN + CW_MSG_LEN, pk) != 0) return -1;
    if (outlen != CW_MSG_LEN) return -1;
    *cmd_out = cmd;
    return 0;
}
```

- [ ] **Step 5: Ensure `config_wire.c` builds**

Add `"config_wire.c"` to `components/simulacra_radar/CMakeLists.txt` SRCS if it lists sources explicitly.

- [ ] **Step 6: Rebuild selftest, verify PASS**

Run: `idf.py -DCHURN_SELFTEST=1 build flash monitor` (C5)
Expected: `SELFTEST: PASS (n/n)` — including the tamper/nonce-mismatch checks.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/config_wire.h components/simulacra_radar/config_wire.c components/simulacra_radar/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(wire): signed CONFIG frame (Ed25519 over nonce||cmd)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Control keypair — generator + committed placeholder headers + build gate

Generate an Ed25519 keypair; emit the public key header (decoys) and secret key header (Vigil), mirroring how `radar_key.h` ships a non-secret placeholder. Add the `SIMULACRA_CONFIG_CTRL` gate to both CMake forwarders.

**Files:**
- Create: `tools/gen_ctrl_key.py`
- Create: `components/simulacra_radar/sim_ctrl_key.h` (generated: `SIMULACRA_CTRL_PK[32]`)
- Create: `cyd/main/sim_ctrl_sk.h` (generated: `SIMULACRA_CTRL_SK[64]`)
- Modify: `main/CMakeLists.txt` and `cyd/main/CMakeLists.txt` (forward `SIMULACRA_CONFIG_CTRL`)

**Interfaces:**
- Produces: `static const uint8_t SIMULACRA_CTRL_PK[32]` (decoy verify key) and `static const uint8_t SIMULACRA_CTRL_SK[64]` (Vigil signing key: seed32‖pub32, TweetNaCl format).

- [ ] **Step 1: Create `tools/gen_ctrl_key.py`**

```python
#!/usr/bin/env python3
"""Generate an Ed25519 control keypair for the Vigil->decoy CONFIG link.
Emits a public-key header for decoys and a secret-key header for Vigil (TweetNaCl 64-byte sk).
Requires the 'cryptography' package (already present in the ESP-IDF Python env)."""
import textwrap
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

def c_array(b):
    hexs = ", ".join(f"0x{x:02x}" for x in b)
    return "\n".join("    " + line for line in textwrap.wrap(hexs, 96))

priv = Ed25519PrivateKey.generate()
seed = priv.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                          serialization.NoEncryption())                       # 32-byte seed
pub  = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)  # 32
sk   = seed + pub                                                             # TweetNaCl sk = seed||pub (64)

with open("components/simulacra_radar/sim_ctrl_key.h", "w") as f:
    f.write("#pragma once\n#include <stdint.h>\n"
            "// Ed25519 PUBLIC key for the Vigil->decoy CONFIG link. Non-secret placeholder;\n"
            "// regenerate with tools/gen_ctrl_key.py before real use. Decoys verify with this.\n"
            f"static const uint8_t SIMULACRA_CTRL_PK[32] = {{\n{c_array(pub)}\n}};\n")

with open("cyd/main/sim_ctrl_sk.h", "w") as f:
    f.write("#pragma once\n#include <stdint.h>\n"
            "// Ed25519 SECRET key (seed||pub) for Vigil. CHANGE ME before real use; keep off the\n"
            "// telemetry-only nodes. TweetNaCl 64-byte secret-key format.\n"
            f"static const uint8_t SIMULACRA_CTRL_SK[64] = {{\n{c_array(sk)}\n}};\n")

print("wrote components/simulacra_radar/sim_ctrl_key.h and cyd/main/sim_ctrl_sk.h")
```

- [ ] **Step 2: Run the generator**

Run: `python tools/gen_ctrl_key.py` (from repo root, in the ESP-IDF Python env).
Expected: both headers written; `sim_ctrl_key.h` contains a 32-byte array, `sim_ctrl_sk.h` a 64-byte array.

- [ ] **Step 3: Verify the placeholder keypair is self-consistent**

Add a temporary check to `test_config_wire` (then remove after this step): sign with `SIMULACRA_CTRL_SK` and verify with `SIMULACRA_CTRL_PK` instead of the ephemeral keypair; confirm the round-trip passes on-device. This proves the seed→pub derivation matches TweetNaCl. Restore the ephemeral-keypair version afterward.

Run: `idf.py -DCHURN_SELFTEST=1 build flash monitor`
Expected: the round-trip `ST_CHECK`s pass.

- [ ] **Step 4: Add `SIMULACRA_CONFIG_CTRL` to both CMake forwarders**

In `main/CMakeLists.txt`, add `SIMULACRA_CONFIG_CTRL` to the `foreach` list of forwarded `-D` flags (same pattern as `SIMULACRA_ESPNOW`). Do the same in `cyd/main/CMakeLists.txt`.

- [ ] **Step 5: Verify the gate forwards**

Run: `idf.py -DSIMULACRA_CONFIG_CTRL=1 -DCHURN_SELFTEST=1 build` and confirm no CMake error; the macro is available to sources (used in Tasks 7 and 11).

- [ ] **Step 6: Commit**

```bash
git add tools/gen_ctrl_key.py components/simulacra_radar/sim_ctrl_key.h cyd/main/sim_ctrl_sk.h main/CMakeLists.txt cyd/main/CMakeLists.txt
git commit -m "feat(crypto): control keypair generator + placeholder headers + SIMULACRA_CONFIG_CTRL gate

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: Decoy receive path — handle CONFIG frames

Accept `RADAR_TYPE_CONFIG` in the decoy: replay-gate, reconstruct the nonce, Ed25519-verify, then `sim_settings_apply_preset`. Silent drop on any failure.

**Files:**
- Modify: `main/esp_now_link.c` (new replay slot + CONFIG branch in `on_recv`)
- Test: on-target (two-board), plus a small pure helper unit test.

**Interfaces:**
- Consumes: Task 5 `config_wire_open_signed`, `RADAR_TYPE_CONFIG`, `config_cmd_t`; Task 6 `SIMULACRA_CTRL_PK`; Task 3 `sim_settings_apply_preset`; existing `radar_replay_ok`, `radar_wire_open` (returns `out_salt[4]`, `out_counter`).

- [ ] **Step 1: Add the CONFIG branch to `on_recv` in `main/esp_now_link.c`**

Add includes near the existing ones:

```c
#include "config_wire.h"
#include "sim_ctrl_key.h"
#include "settings.h"
```

Add a replay slot beside the others (near `s_sig_replay`):

```c
static radar_replay_t s_cfg_replay;   // reject replayed CONFIG from Vigil
```

Add the branch inside `on_recv`, after the `RADAR_TYPE_FLEET_MACS` block (still inside the function, using the already-decoded `type`, `pl`, `plen`, `salt`, `ctr`):

```c
#ifdef SIMULACRA_CONFIG_CTRL
    if (type == RADAR_TYPE_CONFIG) {
        if (!radar_replay_ok(&s_cfg_replay, salt, ctr)) return;   // replayed command
        uint8_t nonce12[12];                                      // salt(4) || counter(8 BE)
        memcpy(nonce12, salt, 4);
        for (int i = 0; i < 8; i++) nonce12[4 + i] = (uint8_t)(ctr >> (56 - 8 * i));
        config_cmd_t cmd;
        if (config_wire_open_signed(pl, plen, nonce12, SIMULACRA_CTRL_PK, &cmd) != 0) return;  // bad sig
        if (cmd.version != CONFIG_WIRE_VER) return;
        if (sim_settings_apply_preset((sim_preset_t)cmd.preset_id) == 0)
            ESP_LOGW(ETAG, "config: applied preset %u", (unsigned)cmd.preset_id);
        return;
    }
#endif
```

> Note: `nonce12` must match how `radar_wire_seal` builds the nonce (salt(4) ‖ counter 8-byte **big-endian**). If a build/verify mismatch appears in Step 3, confirm the counter byte order against `radar_wire_seal` in `radar_wire.c` and align this loop to it.

- [ ] **Step 2: Build the decoy image with control enabled**

Run: `idf.py -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1 -DSIMULACRA_CONFIG_CTRL=1 build flash monitor` (C5, COM15)
Expected: boots as decoy (`config AP up: SSID=simulacra-...`), no crash.

- [ ] **Step 3: On-target apply check (needs Vigil send from Task 11 — defer verification)**

This branch is verified end-to-end once Task 11 lands (Vigil SEND). For now confirm the image runs and ignores malformed traffic (send a random ESP-NOW frame via the sniffer build; decoy must not apply anything). Record: no spurious "config: applied" log.

- [ ] **Step 4: Commit**

```bash
git add main/esp_now_link.c
git commit -m "feat(espnow): decoy applies signed CONFIG presets (verify + clamp via settings)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: Web UI — route controls through settings + preset control

Route the existing web-UI pause toggle through the settings backend and add a preset selector, so both writers (web UI + Vigil) share one backend and NVS state.

**Files:**
- Modify: `main/webui.c:115-117` (control handler)
- Modify: `main/webui_index.html` (preset buttons — optional but included for parity)
- Test: `main/churn_selftest.c` (`test_webui_json` already exists; add a control-parse assertion via a small pure helper if the handler is refactored — otherwise on-target).

**Interfaces:**
- Consumes: Task 3 `sim_settings_apply_preset`, `sim_settings_get`; `sim_preset_t`.

- [ ] **Step 1: Route pause + add preset commands in `main/webui.c`**

Add `#include "settings.h"`. Replace the churn-toggle line and add preset handling in `h_control` (around line 115-117):

```c
    if      (strstr(body, "detect_toggle")) detect_set_enabled(!detect_enabled());
    else if (strstr(body, "churn_toggle"))  sim_settings_apply_preset(
                                                sim_settings_get_paused() ? SIM_PRESET_NORMAL : SIM_PRESET_PAUSE);
    else if (strstr(body, "clear_threats")) detect_clear_threats();
    else if (strstr(body, "preset_stealth")) sim_settings_apply_preset(SIM_PRESET_STEALTH);
    else if (strstr(body, "preset_normal"))  sim_settings_apply_preset(SIM_PRESET_NORMAL);
    else if (strstr(body, "preset_dense"))   sim_settings_apply_preset(SIM_PRESET_DENSE);
    else if (strstr(body, "preset_max"))     sim_settings_apply_preset(SIM_PRESET_MAX);
    else if (strstr(body, "preset_pause"))   sim_settings_apply_preset(SIM_PRESET_PAUSE);
```

Add a tiny helper to `settings.c`/`.h` used above (keeps webui decoupled from the struct):

```c
// settings.h
bool sim_settings_get_paused(void);
// settings.c
bool sim_settings_get_paused(void) { return s_cur.paused; }
```

- [ ] **Step 2: Add preset buttons to `main/webui_index.html`**

Next to the existing control buttons, add a preset row (matches the page's existing POST-to-`/api/control` pattern — copy an existing button's JS and change the body string):

```html
<div class="row">
  <button onclick="ctl('preset_stealth')">Stealth</button>
  <button onclick="ctl('preset_normal')">Normal</button>
  <button onclick="ctl('preset_dense')">Dense</button>
  <button onclick="ctl('preset_max')">Max</button>
  <button onclick="ctl('preset_pause')">Pause</button>
</div>
```

(Use whatever the existing helper is named; if the page posts inline, mirror that exact call.)

- [ ] **Step 3: Build + on-target check**

Run: `idf.py -DCHURN_SELFTEST=0 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1 -DSIMULACRA_CONFIG_CTRL=1 build flash monitor` (C5)
Open the config-window dashboard, tap **Dense**, confirm `/api/status` shows `active_target` unchanged (16) but the decoy log reflects the preset apply; tap **Stealth**, confirm `active_target` drops to 6. Reboot, confirm the last preset persists (NVS).

- [ ] **Step 4: Commit**

```bash
git add main/webui.c main/webui_index.html main/settings.c main/settings.h
git commit -m "feat(webui): presets + pause routed through the shared settings backend

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 9: Vigil — XPT2046 touch coordinates (bit-banged)

Add coordinate reads on top of the existing IRQ press-detect. No free SPI host (SPI2=display, SPI3=SD), so bit-bang the XPT2046 on its dedicated CYD pins. Coarse zones only, so calibration is approximate.

**Files:**
- Modify: `cyd/main/cyd_main.c` (pin defines + `touch_read_xy`)

**Interfaces:**
- Produces: `static bool touch_read_xy(int *x, int *y);` — returns true with pixel coords [0..LCD_W)×[0..LCD_H) while pressed, false when not pressed.
- CYD touch pins (ESP32-2432S028R): T_CLK=25, T_CS=33, T_DIN(MOSI)=32, T_DOUT(MISO)=39, T_IRQ=36 (existing).

- [ ] **Step 1: Add pin defines + GPIO init**

Near the existing `#define TOUCH_IRQ_GPIO 36` (line 43):

```c
#define TOUCH_CLK_GPIO 25
#define TOUCH_CS_GPIO  33
#define TOUCH_DIN_GPIO 32
#define TOUCH_DOUT_GPIO 39     // input-only pin, OK for MISO
```

Extend `touch_init()` to configure CLK/CS/DIN as outputs and DOUT as input:

```c
    gpio_config_t oc = { .pin_bit_mask = (1ULL<<TOUCH_CLK_GPIO)|(1ULL<<TOUCH_CS_GPIO)|(1ULL<<TOUCH_DIN_GPIO),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&oc);
    gpio_config_t ic = { .pin_bit_mask = 1ULL<<TOUCH_DOUT_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&ic);
    gpio_set_level(TOUCH_CS_GPIO, 1);   // idle high
```

- [ ] **Step 2: Add the bit-banged reader**

Add above `draw_freshness_overlay`:

```c
// Bit-banged XPT2046 read (SPI mode 0). cmd 0x90 = X, 0xD0 = Y (12-bit, single-ended).
static uint16_t xpt_xfer(uint8_t cmd)
{
    for (int i = 7; i >= 0; i--) {                       // send command MSB-first
        gpio_set_level(TOUCH_DIN_GPIO, (cmd >> i) & 1);
        gpio_set_level(TOUCH_CLK_GPIO, 1); gpio_set_level(TOUCH_CLK_GPIO, 0);
    }
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {                        // read 16 clocks, take top 12 bits
        gpio_set_level(TOUCH_CLK_GPIO, 1);
        v = (uint16_t)((v << 1) | (gpio_get_level(TOUCH_DOUT_GPIO) & 1));
        gpio_set_level(TOUCH_CLK_GPIO, 0);
    }
    return v >> 4;
}

static bool touch_read_xy(int *x, int *y)
{
    if (gpio_get_level(TOUCH_IRQ_GPIO)) return false;    // not pressed (idle high)
    gpio_set_level(TOUCH_CS_GPIO, 0);
    uint16_t rx = xpt_xfer(0x90);
    uint16_t ry = xpt_xfer(0xD0);
    gpio_set_level(TOUCH_CS_GPIO, 1);
    if (rx < 100 || ry < 100) return false;              // reject noise/no-contact
    // Coarse calibration: raw ~[200..3900] -> pixels. Panel is portrait 240x320.
    int px = (int)((rx - 200) * LCD_W / 3700); if (px < 0) px = 0; if (px >= LCD_W) px = LCD_W-1;
    int py = (int)((ry - 200) * LCD_H / 3700); if (py < 0) py = 0; if (py >= LCD_H) py = LCD_H-1;
    *x = px; *y = py;
    return true;
}
```

- [ ] **Step 3: Build + log-verify coordinates**

Temporarily log coords in the main loop: `int tx,ty; if (touch_read_xy(&tx,&ty)) ESP_LOGW(TAG,"touch %d,%d",tx,ty);`
Run: `cd cyd && idf.py -DSIMULACRA_CONFIG_CTRL=1 build flash monitor` (CYD, COM10)
Expected: tapping the four corners logs coords that move roughly corner-to-corner (exact calibration not required — only that top/bottom and left/right are distinguishable). Remove the temporary log after confirming.

- [ ] **Step 4: Commit**

```bash
git add cyd/main/cyd_main.c
git commit -m "feat(vigil): bit-banged XPT2046 touch coordinates (coarse zones)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 10: Vigil — CONTROL view + input model

Add `RADAR_VIEW_CONTROL` to the shared view enum and a small input model: which preset is highlighted, and a send-flash timer. Keep it pure/testable in the shared `radar_ui` unit.

**Files:**
- Modify: `components/simulacra_radar/radar_ui.h` (enum + control state)
- Modify: `components/simulacra_radar/radar_ui.c` (init/select helpers)
- Test: `main/churn_selftest.c` (extend `test_radar_ui`)

**Interfaces:**
- Produces:
  - enum gains `RADAR_VIEW_CONTROL` (before `RADAR_VIEW_COUNT`).
  - `radar_ui_t` gains `uint8_t sel_preset; uint32_t send_flash_ms;`
  - `void radar_ctrl_select_next(radar_ui_t *ui);` (cycle highlighted preset 0..4)
  - `void radar_ctrl_mark_sent(radar_ui_t *ui, uint32_t now_ms);` (start the SENT flash)
  - `#define RADAR_CTRL_PRESET_COUNT 5`, `#define RADAR_CTRL_FLASH_MS 1200u`

- [ ] **Step 1: Write the failing test (extend `test_radar_ui`)**

Append to `test_radar_ui` in `main/churn_selftest.c`:

```c
    // CONTROL view joins the cycle and has an independent preset selector.
    radar_ui_reset(&ui, 20000, 0);
    for (int i = 0; i < RADAR_VIEW_CONTROL; i++) radar_ui_on_input(&ui, 20000 + i*10);
    ST_CHECK(ui.view == RADAR_VIEW_CONTROL, "cycle reaches CONTROL");
    uint8_t p0 = ui.sel_preset;
    radar_ctrl_select_next(&ui);
    ST_CHECK(ui.sel_preset == (p0 + 1) % RADAR_CTRL_PRESET_COUNT, "select_next advances preset");
    radar_ctrl_mark_sent(&ui, 21000);
    ST_CHECK(ui.send_flash_ms == 21000, "mark_sent stamps the flash");
```

- [ ] **Step 2: Run selftest, verify fail**

Run: `idf.py -DCHURN_SELFTEST=1 build`
Expected: FAIL — `RADAR_VIEW_CONTROL` / `radar_ctrl_select_next` undefined.

- [ ] **Step 3: Extend `components/simulacra_radar/radar_ui.h`**

Add to the enum (before `RADAR_VIEW_COUNT`): `RADAR_VIEW_CONTROL,`
Add fields to `radar_ui_t`: `uint8_t sel_preset; uint32_t send_flash_ms;`
Add near the other defines:

```c
#define RADAR_CTRL_PRESET_COUNT 5
#define RADAR_CTRL_FLASH_MS     1200u
void radar_ctrl_select_next(radar_ui_t *ui);
void radar_ctrl_mark_sent(radar_ui_t *ui, uint32_t now_ms);
```

- [ ] **Step 4: Implement in `components/simulacra_radar/radar_ui.c`**

In `radar_ui_reset`, initialize the new fields: `ui->sel_preset = 2; ui->send_flash_ms = 0;` (default highlight = NORMAL).
Add:

```c
void radar_ctrl_select_next(radar_ui_t *ui)
{ ui->sel_preset = (uint8_t)((ui->sel_preset + 1) % RADAR_CTRL_PRESET_COUNT); }
void radar_ctrl_mark_sent(radar_ui_t *ui, uint32_t now_ms) { ui->send_flash_ms = now_ms; }
```

- [ ] **Step 5: Rebuild selftest, verify PASS**

Run: `idf.py -DCHURN_SELFTEST=1 build flash monitor` (C5)
Expected: `SELFTEST: PASS (n/n)`.

> Note: adding `RADAR_VIEW_CONTROL` grows the view cycle by one; the existing "idle returns to radar" test still holds because it checks the terminal state, not the count.

- [ ] **Step 6: Commit**

```bash
git add components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_ui.c main/churn_selftest.c
git commit -m "feat(vigil): CONTROL view + preset selector/flash input model

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 11: Vigil — render CONTROL page + send signed CONFIG

Draw the `◀ [PRESET] ▶` selector + SEND button + SENT flash, wire touch zones on the CONTROL page, and broadcast a signed CONFIG on SEND.

**Files:**
- Modify: `components/simulacra_radar/radar_render.h` (add `radar_ctrl_info_t` + param)
- Modify: `components/simulacra_radar/radar_render.c` (add `draw_control`)
- Modify: `cyd/main/cyd_main.c` (CONFIG send + control-page touch handling + pass ctrl info to render)

**Interfaces:**
- Consumes: Task 9 `touch_read_xy`; Task 10 `RADAR_VIEW_CONTROL`, `radar_ctrl_select_next`, `radar_ctrl_mark_sent`, `sel_preset`, `send_flash_ms`; Task 5 `config_wire_pack_signed`, `RADAR_TYPE_CONFIG`, `config_cmd_t`; Task 6 `SIMULACRA_CTRL_SK`.
- Produces: `typedef struct { uint8_t sel_preset; bool send_flash; } radar_ctrl_info_t;` and a `radar_render_view` that accepts it.

- [ ] **Step 1: Add `radar_ctrl_info_t` + render param in `radar_render.h`**

```c
typedef struct { uint8_t sel_preset; bool send_flash; } radar_ctrl_info_t;   // CONTROL page state
```

Extend `radar_render_view` signature to take `const radar_ctrl_info_t *ctrl` (NULL on non-Vigil / non-control). Update the prototype and the doc comment.

- [ ] **Step 2: Implement `draw_control` + dispatch in `radar_render.c`**

Add preset labels and the drawer:

```c
static const char *CTRL_LABELS[5] = { "PAUSE", "STEALTH", "NORMAL", "DENSE", "MAX" };
static void draw_control(radar_gfx_t *g, const radar_ctrl_info_t *c)
{
    radar_gfx_text(g, 8, 6, "CONTROL", COL_FG);
    uint8_t sel = c ? c->sel_preset : 2;
    radar_gfx_text(g, 20, 120, "<", COL_DIM);
    radar_gfx_text(g, 200, 120, ">", COL_DIM);
    char box[16]; snprintf(box, sizeof box, "[ %s ]", CTRL_LABELS[sel % 5]);
    radar_gfx_text(g, 70, 120, box, COL_FG);
    radar_gfx_fill_rect(g, 60, 210, 120, 40, COL_RING);      // SEND button
    radar_gfx_text(g, 96, 224, c && c->send_flash ? "SENT" : "SEND",
                   c && c->send_flash ? COL_OK : COL_FG);
    radar_gfx_text(g, 30, 296, "broadcast to all decoys", COL_DIM);
}
```

In `radar_render_view`, add the dispatch branch and thread `ctrl`:

```c
        else if (view == RADAR_VIEW_CONTROL) draw_control(&g, ctrl);
```

- [ ] **Step 3: CONFIG send + control touch zones in `cyd/main/cyd_main.c`**

Add includes: `#include "config_wire.h"` and (gated) `#include "sim_ctrl_sk.h"`.
Add the sender:

```c
#ifdef SIMULACRA_CONFIG_CTRL
static void send_config(uint8_t preset)
{
    uint64_t ctr = ++s_ctr;
    uint8_t nonce12[12]; memcpy(nonce12, s_salt, 4);
    for (int i = 0; i < 8; i++) nonce12[4+i] = (uint8_t)(ctr >> (56 - 8*i));
    config_cmd_t cmd = { .version = CONFIG_WIRE_VER, .preset_id = preset };
    uint8_t pl[CONFIG_WIRE_PAYLOAD_LEN];
    if (config_wire_pack_signed(pl, sizeof pl, &cmd, nonce12, SIMULACRA_CTRL_SK) < 0) return;
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    if (radar_wire_seal(frame, &flen, RADAR_TYPE_CONFIG, pl, sizeof pl,
                        SIMULACRA_ESPNOW_KEY, s_salt, ctr) == 0)
        for (int i = 0; i < 4; i++) esp_now_send(BCAST, frame, flen);
    ESP_LOGW(TAG, "sent CONFIG preset %u", (unsigned)preset);
}
#endif
```

In the main loop, replace the single `touched()` handling so the CONTROL page uses zones:

```c
        int tx, ty;
        bool press = touch_read_xy(&tx, &ty);
        static bool was_press = false;
        bool edge = press && !was_press;                 // fresh contact
        was_press = press;
        if (edge) {
            if (ui.view == RADAR_VIEW_CONTROL) {
                radar_ui_note_input(&ui, now);           // keep backlight/idle timer fresh
#ifdef SIMULACRA_CONFIG_CTRL
                if (ty > 200 && tx > 60 && tx < 180) {   // SEND button
                    send_config(ui.sel_preset);
                    radar_ctrl_mark_sent(&ui, now);
                } else if (tx < 80) {                    // left zone: prev == cycle-around
                    for (int i = 0; i < RADAR_CTRL_PRESET_COUNT - 1; i++) radar_ctrl_select_next(&ui);
                } else if (tx > 160) {                   // right zone: next
                    radar_ctrl_select_next(&ui);
                } else {                                 // center tap = leave to next view
                    radar_ui_on_input(&ui, now); send_request(); last_req = now;
                }
#else
                radar_ui_on_input(&ui, now); send_request(); last_req = now;
#endif
            } else {
                radar_ui_on_input(&ui, now); send_request(); last_req = now;
            }
        }
```

Add a tiny non-cycling input helper to `radar_ui.{h,c}` so the CONTROL page can refresh timers without changing the view:

```c
// radar_ui.h
void radar_ui_note_input(radar_ui_t *ui, uint32_t now_ms);
// radar_ui.c
void radar_ui_note_input(radar_ui_t *ui, uint32_t now_ms)
{ ui->last_input_ms = now_ms; ui->last_wake_ms = now_ms; ui->backlight_on = true; }
```

Update the render call to pass ctrl info:

```c
        radar_ctrl_info_t ctrl = { .sel_preset = ui.sel_preset,
            .send_flash = (ui.send_flash_ms && (now - ui.send_flash_ms) < RADAR_CTRL_FLASH_MS) };
        radar_render_view(ui.view, &s_status, &lib, &ctrl, sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
```

(Adjust the existing `radar_render_view` call site to the new signature; other non-Vigil callers pass `NULL` for `ctrl`.)

- [ ] **Step 4: Update the selftest render call site (if any) + build both**

The selftest / any other `radar_render_view` caller must pass the new `ctrl` arg (NULL). Grep for `radar_render_view(` and fix each.
Run: `idf.py -DCHURN_SELFTEST=1 build` (decoy) and `cd cyd && idf.py -DSIMULACRA_CONFIG_CTRL=1 build flash monitor` (CYD).
Expected: both build; CYD boots.

- [ ] **Step 5: End-to-end on-target (two boards)**

- Flash the decoy from Task 7 (`-DSIMULACRA_CONFIG_CTRL=1`).
- On Vigil, cycle to CONTROL, tap right to select **STEALTH**, tap **SEND** → "SENT" flashes.
- Decoy serial logs `config: applied preset 1`; its STATUS (visible on Vigil STATS page) shows `active_target` → 6.
- Send **MAX** → decoy log `applied preset 4`; on a C6 decoy confirm target clamps to its ceiling.
- Flip one byte of the Vigil `SIMULACRA_CTRL_SK` in a scratch build → decoy ignores (no "applied" log). Restore.
- Reboot the decoy → last preset persists (NVS from Task 3).

- [ ] **Step 6: Commit**

```bash
git add components/simulacra_radar/radar_render.h components/simulacra_radar/radar_render.c components/simulacra_radar/radar_ui.h components/simulacra_radar/radar_ui.c cyd/main/cyd_main.c main/churn_selftest.c
git commit -m "feat(vigil): CONTROL page renders selector/SEND and broadcasts signed CONFIG

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 12: Docs + roadmap

Reflect the shipped control link in the README and memory.

**Files:**
- Modify: `README.md` (Vigil section — note fleet control console)
- Modify: memory `MEMORY.md` + `vigil-remote-control.md` (mark done)

- [ ] **Step 1: Update README**

In the Vigil paragraph, add one or two sentences: Vigil can now push behaviour presets (PAUSE/STEALTH/NORMAL/DENSE/MAX) to the fleet over the encrypted ESP-NOW link; commands are Ed25519-signed (decoys hold only the public key) and clamped so a forged command can't degrade a decoy below the STEALTH floor. Keep the existing voice.

- [ ] **Step 2: Update memory**

Edit `vigil-remote-control.md`: change status from "future/not started" to shipped, with the branch/commit and the key facts (Ed25519 + clamp, preset ladder, NVS-backed settings backend shared with web UI). Update the `MEMORY.md` one-liner.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs(readme): Vigil fleet control console (signed preset push)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

(Memory files live outside the repo; save them via the memory tooling, not this commit.)

---

## Self-Review

**Spec coverage:**
- Runtime settings backend (RAM+NVS) → Tasks 2, 3. ✓
- Dwell/cooldown runtime-mutable → Task 1. ✓
- One backend, two writers (web UI + CONFIG) → Tasks 3, 7, 8. ✓
- Preset ladder resolving against each board's ceiling → Task 2 (`sim_settings_resolve` + ceiling). ✓
- Button CONTROL page + coordinates → Tasks 9, 10, 11. ✓
- Ed25519-signed CONFIG over nonce‖cmd → Tasks 4, 5. ✓
- Safe-bounds clamp → Task 2 (`sim_settings_clamp`, applied on every path). ✓
- Key provisioning + `SIMULACRA_CONFIG_CTRL` gate → Task 6. ✓
- Broadcast to all decoys → Task 11 `send_config`. ✓
- Testing (map/clamp/pack/verify/tamper + on-target apply/persist/forgery) → Tasks 2, 3, 5, 7, 11. ✓
- Frame type 7, silent drop on failure → Tasks 5, 7. ✓

**Scope note:** the spec's "probe cadence" knob is realized via churn turnover (accel + dwell) rather than a new coexist hook; an explicit probe-cadence setting stays deferred (spec "deferred" section). No behaviour promised by the ladder table is lost — DENSE/MAX still increase distinct-device turnover.

**Placeholder scan:** no TBD/TODO; every code step carries real code. The only approximate value is the touch calibration constant (Task 9), which is intentionally coarse and verified by log in Step 3.

**Type consistency:** `sim_settings_t`, `sim_preset_t`, `config_cmd_t`, `radar_ctrl_info_t`, and the `radar_render_view`/`radar_ui_*` signatures are defined once and consumed with matching names/types across tasks. `nonce12` construction (salt‖counter BE) is identical in Task 7 (decoy) and Task 11 (Vigil).

**Verification gap:** Task 7's decoy branch is only exercised end-to-end in Task 11 (needs Vigil to send); this is called out in both tasks. Acceptable — no earlier task can send a signed frame.
