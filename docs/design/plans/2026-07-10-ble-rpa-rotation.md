# BLE Persistent Devices + Per-Type Address Rotation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fixed-identity BLE decoy (one address held for a short dwell) with a persistent-device model whose per-type address rotation (RPA ~15 min, NRPA faster, static none) and transient/resident lifetime split match how real BLE devices behave over time.

**Architecture:** A new pure, host-testable core `main/ble_devices.c` owns the persistent-device population: each device embeds an `identity_t` (its advertising behaviour, drawn from the existing `roster` behaviour library) plus a subtype, a role, a bounded lifetime, and a rotation schedule. It answers, per tick, which devices are live and what address each currently presents. `main/churn.c` is refactored from an identity-lifecycle owner into a pure **presenter**: it maps the live `ble_devices` population onto the 4 hardware advertising instances and re-applies an instance when its occupant's address changes (a rotation). Content generation (`templates`/`generate`/`roster`) and NimBLE apply (`churn_adv`) are reused unchanged.

**Tech Stack:** C (ESP-IDF 5.5 on ESP32-C5, 5.4 on ESP32-C6, NimBLE ext-adv), MSVC `cl` / gcc host build for the pure core under `tools/decoy_audit`, Python `unittest` for host tests, an nRF/Kismet BLE capture (pcap) for on-air validation.

## Global Constraints

- **Public repo** — no absolute local paths, OS usernames, real hardware MACs, or real SSIDs in any committed file. Captures and every parsed intermediate stay in the gitignored `private/`, PII-stripped; `decoy_audit` outputs remain aggregate-only.
- **Targets** — ESP32-C5 (IDF 5.5) and ESP32-C6 (IDF 5.4); classic ESP32 CYD unaffected. The device core is target-agnostic; per-target population size stays runtime (`generate_active_target`), clamped to `BLE_DEVICES_MAX`.
- **Reuse, don't rewrite** — content generation (`templates`/`generate`/`roster`) and the NimBLE apply (`churn_adv`) are reused; only the lifecycle/address layer is replaced. All existing `decoy_audit` snapshot discriminators (interval / vendor / address-type mix) must stay green — this milestone must not regress them.
- **Host-testable core** — all new scheduling/rotation logic lives in `ble_devices.c` with RNG via `esp_random()` (real on target, stubbed to `rand()` on host) and the clock passed in as `now_ms`. No NimBLE/ESP-IDF calls in the pure core.
- **All three subtypes are random addresses** — rotation regenerates only the low 46 bits via `make_random_addr(addr, top2)`; the subtype MSBs are preserved and no real hardware MAC is ever exposed.
- **Honest ceiling** — this defeats the temporal "never rotates" / lifetime-monoculture tells. It does NOT defeat physical co-location / RSSI / AoA; a rotated device stays relinkable across its own rotations (like a real device — unlinkability comes from death); cadence must stay realistic (never cranked for volume). Do not describe it as invisibility in any artifact.
- **Commit trailer** — end every commit message with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`

---

## File Structure

- `main/ble_devices.h` (new) — `ble_device_t` + pure device-population API.
- `main/ble_devices.c` (new) — the pure core: spawn (subtype blend + role split + behaviour draw + address), per-type rotation, death/rebirth lifecycle. Host-buildable.
- `main/churn.c` (modify) — refactor from identity-lifecycle owner to presenter: source the active set from `ble_devices`, re-apply an instance on address change. Keep the public API (`churn_active_count/at`, `churn_set_active_target`, `churn_set_paused`).
- `main/churn.h` (modify) — doc the presenter role; retain the existing setter declarations.
- `main/simulacra_main.c` (modify) — call `ble_devices_init` after `roster_init` and before `churn_init`.
- `main/CMakeLists.txt` (modify) — add `ble_devices.c` to `SRCS`.
- `tools/decoy_audit/synth_dump.c` (modify) — add a `--devices` time-series dumper subcommand.
- `tools/decoy_audit/run.ps1` (modify) + `tools/decoy_audit/Makefile` (modify) — add `ble_devices.c` to the host build source list.
- `tools/decoy_audit/tests/test_ble_devices.py` (new) — host unit tests for the pure core.
- `tools/decoy_audit/analyzers/rotation_audit.py`, `analyzers/README.md` (new) — permanent temporal validator (rotation cadence / lifetime cohort / independence) over a `--devices` run; optional presence-duration comparison to a real capture profile.
- `tools/decoy_audit/capture_profile.py` (modify) — emit an aggregate per-address presence-duration histogram into `profile.json` (privacy-safe) for the optional real-vs-synth temporal comparison.

---

## Task 1: Pure core — module scaffold + spawn (TDD)

Create the pure device core with spawn only (subtype blend, role split, behaviour drawn from the roster library, address from the chosen subtype). Wire it into the host harness and prove spawn distributions with unit tests. Rotation and lifecycle are stubs here.

**Files:**
- Create: `main/ble_devices.h`, `main/ble_devices.c`
- Modify: `tools/decoy_audit/synth_dump.c`, `tools/decoy_audit/run.ps1`, `tools/decoy_audit/Makefile`
- Test: `tools/decoy_audit/tests/test_ble_devices.py`

**Interfaces:**
- Consumes: `identity_t` (`main/identity.h`); `roster_init`, `roster_at`, `CHURN_ROSTER_SIZE`, `make_random_addr` (`main/roster.h`); `esp_random` (host-stubbed).
- Produces: `ble_device_t`, `ble_atype_t`, `ble_role_t`, `ble_devices_init(int n, uint32_t now_ms)`, `ble_devices_tick(uint32_t now_ms)` (stub returns immediately here), `ble_devices_count(void)`, `ble_devices_at(int i)`.

- [ ] **Step 1: Write the header.** Create `main/ble_devices.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "identity.h"

// Max concurrent persistent devices per board. The runtime population (set at init) is
// clamped to this. Perceived density comes from turnover, not from raising this ceiling.
#define BLE_DEVICES_MAX 32

typedef enum { BLE_ATYPE_STATIC, BLE_ATYPE_RPA, BLE_ATYPE_NRPA } ble_atype_t;
typedef enum { BLE_ROLE_TRANSIENT, BLE_ROLE_RESIDENT } ble_role_t;

typedef struct {
    identity_t  id;             // advertising identity: addr + frozen behaviour (payload/itvl/tx/company/arch)
    ble_atype_t atype;          // fixed for life; selects rotation policy (STATIC never rotates)
    ble_role_t  role;           // fixed for life; selects the lifetime band
    uint32_t    born_ms;        // set at spawn; == now on a fresh birth/rebirth
    uint32_t    life_ms;        // bounded lifetime; on expiry the device dies and is reborn fresh
    uint32_t    next_rotate_ms; // next address rotation (ignored for STATIC)
    bool        alive;
} ble_device_t;

// Spawn `n` persistent devices (clamped to BLE_DEVICES_MAX). Behaviour is drawn from the
// roster library, so roster_init() MUST have been called first.
void  ble_devices_init(int n, uint32_t now_ms);
// One scheduler tick: retire+reincarnate expired devices, then rotate the address of any
// rotating-subtype device whose next_rotate_ms has passed. Behaviour is preserved across a
// rotation; only addr changes.
void  ble_devices_tick(uint32_t now_ms);
int   ble_devices_count(void);
const ble_device_t *ble_devices_at(int i);
```

- [ ] **Step 2: Write the implementation (spawn only; tick is a stub).** Create `main/ble_devices.c`:

```c
#include "ble_devices.h"
#include "roster.h"
#include "esp_random.h"
#include <string.h>

// Role split (user-chosen): ~70% transient / ~30% resident.
#define ROLE_RESIDENT_PCT   30
// Address-subtype blend, matching roster.c's calibrated fleet mix (~52/36/12 static/RPA/NRPA).
#define ATYPE_STATIC_W  52
#define ATYPE_RPA_W     36
#define ATYPE_NRPA_W    12
// Lifetime bands.
#define TRANSIENT_MIN_MS   120000u    // 2 min
#define TRANSIENT_MAX_MS   720000u    // 12 min
#define RESIDENT_MIN_MS   1800000u    // 30 min
#define RESIDENT_MAX_MS   5400000u    // 90 min
// Rotation cadence per subtype (independent phase + wide jitter). STATIC never rotates.
#define RPA_ROT_MIN_MS     600000u    // 10 min
#define RPA_ROT_MAX_MS    1200000u    // 20 min
#define NRPA_ROT_MIN_MS     60000u    // 1 min
#define NRPA_ROT_MAX_MS    600000u    // 10 min

static ble_device_t s_dev[BLE_DEVICES_MAX];
static int          s_n;

static uint32_t rnd_range(uint32_t lo, uint32_t hi) { return lo + (esp_random() % (hi - lo + 1u)); }

static uint8_t top2_for(ble_atype_t a)
{
    switch (a) { case BLE_ATYPE_STATIC: return 0xC0; case BLE_ATYPE_RPA: return 0x40;
                 default: return 0x00; }   // NRPA
}

static ble_atype_t pick_atype(void)
{
    uint32_t r = esp_random() % (ATYPE_STATIC_W + ATYPE_RPA_W + ATYPE_NRPA_W);
    if (r < ATYPE_STATIC_W) return BLE_ATYPE_STATIC;
    if (r < ATYPE_STATIC_W + ATYPE_RPA_W) return BLE_ATYPE_RPA;
    return BLE_ATYPE_NRPA;
}

static uint32_t rotate_base(ble_atype_t a)
{
    switch (a) {
        case BLE_ATYPE_RPA:  return rnd_range(RPA_ROT_MIN_MS,  RPA_ROT_MAX_MS);
        case BLE_ATYPE_NRPA: return rnd_range(NRPA_ROT_MIN_MS, NRPA_ROT_MAX_MS);
        default:             return 0;   // STATIC: unused
    }
}

// Draw a frozen behaviour (payload/itvl/company/tx/archetype) from the roster library and
// stamp a fresh address of the chosen subtype. The roster entry's own address is discarded.
static void dev_spawn(ble_device_t *d, uint32_t now_ms)
{
    identity_t *src = roster_at(esp_random() % CHURN_ROSTER_SIZE);
    d->id = *src;                                   // copy behaviour (and its addr, overwritten next)
    d->atype = pick_atype();
    make_random_addr(d->id.addr, top2_for(d->atype));
    d->role = (esp_random() % 100u < ROLE_RESIDENT_PCT) ? BLE_ROLE_RESIDENT : BLE_ROLE_TRANSIENT;
    d->born_ms = now_ms;
    d->life_ms = (d->role == BLE_ROLE_RESIDENT) ? rnd_range(RESIDENT_MIN_MS, RESIDENT_MAX_MS)
                                                : rnd_range(TRANSIENT_MIN_MS, TRANSIENT_MAX_MS);
    d->alive = true;
    // Independent rotation phase: first rotation is a full jittered interval out from birth.
    d->next_rotate_ms = (d->atype == BLE_ATYPE_STATIC) ? 0 : now_ms + rotate_base(d->atype);
}

void ble_devices_init(int n, uint32_t now_ms)
{
    if (n > BLE_DEVICES_MAX) n = BLE_DEVICES_MAX;
    if (n < 1) n = 1;
    s_n = n;
    for (int i = 0; i < s_n; i++) dev_spawn(&s_dev[i], now_ms);
}

int ble_devices_count(void) { return s_n; }
const ble_device_t *ble_devices_at(int i) { return (i >= 0 && i < s_n) ? &s_dev[i] : 0; }

/* rotation + lifecycle land in Tasks 2-3 */
void ble_devices_tick(uint32_t now_ms) { (void)now_ms; }
```

- [ ] **Step 3: Add the `--devices` dumper subcommand.** In `tools/decoy_audit/synth_dump.c`, add near the other includes: `#include "ble_devices.h"` and `#include "roster.h"`. Then at the very top of `main` (before the existing `seed`/`n` parsing), insert:

```c
    if (argc > 1 && strcmp(argv[1], "--devices") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ndev   = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        int      ticks  = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 4000;
        unsigned tickms = argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 1000;
        srand(seed);
        roster_init();                                  // build the behaviour library (host: template fallback)
        uint32_t t = 0;
        ble_devices_init(ndev, t);
        static uint8_t prev[BLE_DEVICES_MAX][6];
        static int     seen[BLE_DEVICES_MAX];
        memset(seen, 0, sizeof seen);
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            ble_devices_tick(t);
            int cnt = ble_devices_count();
            for (int i = 0; i < cnt; i++) {
                const ble_device_t *d = ble_devices_at(i);
                if (!seen[i] || memcmp(prev[i], d->id.addr, 6) != 0) {
                    const char *ev = (d->born_ms == t) ? "born" : "rotate";
                    const char *at = d->atype == BLE_ATYPE_STATIC ? "static"
                                   : d->atype == BLE_ATYPE_RPA    ? "rpa" : "nrpa";
                    const char *ro = d->role  == BLE_ROLE_RESIDENT ? "resident" : "transient";
                    char hex[13];
                    for (int b = 0; b < 6; b++) sprintf(hex + b*2, "%02x", d->id.addr[b]);
                    printf("D %u %d %s %s %s %s %u %u\n", (unsigned)t, i, hex, at, ro, ev,
                           (unsigned)d->id.company_id, (unsigned)d->id.adv_itvl_ms);
                    memcpy(prev[i], d->id.addr, 6); seen[i] = 1;
                }
            }
        }
        return 0;
    }
```

- [ ] **Step 4: Add the module to both host builds.** In `tools/decoy_audit/run.ps1`, change the `cl` source list (currently `synth_dump.c ble_hs_adv.c learn_stub.c ` + the three `..\..\main\*.c`) to also include `..\..\main\ble_devices.c` — i.e. append it after `..\..\main\roster.c`. In `tools/decoy_audit/Makefile`, change the `SRC :=` line to append `$(ROOT)/main/ble_devices.c` after `$(ROOT)/main/roster.c`.

- [ ] **Step 5: Write the failing tests.** Create `tools/decoy_audit/tests/test_ble_devices.py`:

```python
import os, subprocess, unittest
from collections import defaultdict, Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")

def sim(seed, n=16, ticks=4000, tick_ms=1000):
    out = subprocess.check_output([EXE, "--devices", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 9 and p[0] == "D":
            # t, slot, addr, atype, role, event, company, itvl
            rows.append((int(p[1]), int(p[2]), p[3], p[4], p[5], p[6], int(p[7]), int(p[8])))
    return rows

@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class Spawn(unittest.TestCase):
    def test_initial_births(self):
        rows = sim(1, n=16)
        born0 = [r for r in rows if r[0] == 0 and r[5] == "born"]
        self.assertEqual(len(born0), 16, "expected one initial birth per slot at t=0")

    def test_addr_subtype_matches_atype(self):
        rows = sim(2, n=16)
        self.assertTrue(rows, "no events produced")
        for _, _, addr, at, _, _, _, _ in rows:
            top2 = int(addr[10:12], 16) >> 6   # addr[5] is the last octet (chars 10-11); MSB top-2-bits
            want = {"static": 3, "rpa": 1, "nrpa": 0}[at]
            self.assertEqual(top2, want, f"{at} address {addr} has wrong top-2-bits {top2}")

    def test_subtype_mix_realistic(self):
        rows = [r for r in sim(9, n=32) if r[5] == "born"]   # sample over all births/rebirths
        c = Counter(r[3] for r in rows); tot = sum(c.values())
        self.assertGreater(c["rpa"],  0.20 * tot, "RPA subtype under-represented")
        self.assertGreater(c["nrpa"], 0.03 * tot, "NRPA subtype under-represented")
        self.assertLess(c["static"],  0.75 * tot, "static subtype monoculture")

    def test_role_split_about_70_30(self):
        rows = [r for r in sim(5, n=32) if r[5] == "born"]
        c = Counter(r[4] for r in rows); tot = sum(c.values())
        self.assertGreater(c["transient"], 0.55 * tot, "transient share too low")
        self.assertGreater(c["resident"],  0.15 * tot, "resident share too low")

    def test_behaviour_populated(self):
        rows = sim(3, n=16)
        self.assertTrue(all(itvl > 0 for *_, itvl in rows), "a device has zero advertising interval")
```

- [ ] **Step 6: Build the host harness and run the tests — expect PARTIAL.** Build `synth_dump` with `ble_devices.c` (Developer PowerShell for VS), from `tools/decoy_audit/`:

```
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c learn_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c /Fe:synth_dump.exe
```
Run: `python -m unittest discover -s tools\decoy_audit\tests -v`
Expected: the five `Spawn` tests PASS (spawn is fully implemented), and the existing `test_synth_dump.py` still PASS (the default `synth_dump <seed> <n>` path is untouched). Tick is a stub, but no Task-1 test depends on it.

- [ ] **Step 7: Commit.**
```bash
git add main/ble_devices.h main/ble_devices.c tools/decoy_audit/synth_dump.c tools/decoy_audit/run.ps1 tools/decoy_audit/Makefile tools/decoy_audit/tests/test_ble_devices.py
git commit -m "feat(ble): pure persistent-device core scaffold + spawn (subtype/role/behaviour)"
```

---

## Task 2: Pure core — per-type address rotation (TDD)

Implement address rotation: RPA/NRPA devices rotate on their cadence keeping behaviour; static devices never rotate; rotations are independently phased.

**Files:**
- Modify: `main/ble_devices.c` (implement rotation inside `ble_devices_tick`)
- Test: `tools/decoy_audit/tests/test_ble_devices.py`

**Interfaces:**
- Consumes: `s_dev`, `s_n`, `rotate_base`, `top2_for`, `make_random_addr` (Task 1).
- Produces: address rotation with preserved behaviour; `next_rotate_ms` reschedule.

- [ ] **Step 1: Write the failing tests.** Append to `test_ble_devices.py`:

```python
@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class Rotation(unittest.TestCase):
    def segments(self, rows):
        # group each slot's events into life-segments delimited by 'born'
        by_slot = defaultdict(list)
        for r in sorted(rows, key=lambda r: (r[1], r[0])):
            by_slot[r[1]].append(r)
        segs = []
        for slot, evs in by_slot.items():
            cur = None
            for e in evs:
                if e[5] == "born":
                    if cur: segs.append(cur)
                    cur = [e]
                elif cur:
                    cur.append(e)
            if cur: segs.append(cur)
        return segs

    def test_static_never_rotates(self):
        segs = self.segments(sim(11, n=24, ticks=6000, tick_ms=1000))
        static_segs = [s for s in segs if s[0][3] == "static"]
        self.assertTrue(static_segs, "no static devices sampled")
        for s in static_segs:
            self.assertEqual([e for e in s if e[5] == "rotate"], [], "a static device rotated")

    def test_rpa_rotation_in_band(self):
        segs = self.segments(sim(12, n=24, ticks=8000, tick_ms=1000))
        rots = []
        for s in segs:
            if s[0][3] != "rpa": continue
            ts = [e[0] for e in s if e[5] in ("born", "rotate")]
            rots += [b - a for a, b in zip(ts, ts[1:])]
        self.assertTrue(rots, "no RPA rotations observed")
        # every observed inter-rotation gap sits in the 10-20 min band (ms), allowing tick slack
        for g in rots:
            self.assertGreaterEqual(g, 600000 - 1000, f"RPA rotated too fast: {g} ms")
            self.assertLessEqual(g, 1200000 + 1000, f"RPA rotated too slow: {g} ms")

    def test_behaviour_preserved_across_rotation(self):
        segs = self.segments(sim(13, n=24, ticks=8000, tick_ms=1000))
        rotated = [s for s in segs if any(e[5] == "rotate" for e in s)]
        self.assertTrue(rotated, "no rotations to check continuity on")
        for s in rotated:
            companies = set(e[6] for e in s); itvls = set(e[7] for e in s)
            self.assertEqual(len(companies), 1, "company changed across a rotation")
            self.assertEqual(len(itvls), 1, "interval changed across a rotation")
            addrs = [e[2] for e in s]
            self.assertEqual(len(addrs), len(set(addrs)), "a rotation reused an address")

    def test_rotation_phase_independent(self):
        rows = sim(14, n=24, ticks=8000, tick_ms=1000)
        per_tick_rot = Counter(e[0] for e in rows if e[5] == "rotate")
        self.assertTrue(per_tick_rot, "no rotations observed")
        # no single instant rotates a large share of the population (no synchronized volley)
        self.assertLess(max(per_tick_rot.values()), 24 // 2, "synchronized rotation volley")
```

- [ ] **Step 2: Run — expect FAIL** (tick is a stub, so no `rotate` events exist).

Run: `python -m unittest discover -s tools\decoy_audit\tests -v`
Expected: `test_rpa_rotation_in_band` FAILS on "no RPA rotations observed".

- [ ] **Step 3: Implement rotation.** In `main/ble_devices.c`, replace the `ble_devices_tick` stub with:

```c
void ble_devices_tick(uint32_t now_ms)
{
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (!d->alive) continue;
        if (d->atype == BLE_ATYPE_STATIC) continue;          // static addresses never rotate
        if ((int32_t)(now_ms - d->next_rotate_ms) >= 0) {
            make_random_addr(d->id.addr, top2_for(d->atype)); // new address, SAME subtype
            // behaviour (payload/itvl/company/tx/archetype) is deliberately unchanged
            d->next_rotate_ms = now_ms + rotate_base(d->atype);
        }
    }
}
```

- [ ] **Step 4: Run — expect PASS.**

Run: `python -m unittest discover -s tools\decoy_audit\tests -v`
Expected: all `Rotation` tests PASS (static never rotates, RPA gaps in the 10–20 min band, behaviour preserved, no synchronized volley), and `Spawn` + `test_synth_dump.py` still PASS.

- [ ] **Step 5: Commit.**
```bash
git add main/ble_devices.c tools/decoy_audit/tests/test_ble_devices.py
git commit -m "feat(ble): per-type address rotation (RPA/NRPA rotate, static holds, behaviour preserved)"
```

---

## Task 3: Pure core — death/rebirth lifecycle (TDD)

Implement bounded lifetimes and reincarnation: a device past `born_ms + life_ms` dies and is reborn as a completely fresh device (new subtype/role/behaviour/address/rotation phase). This is the population turnover that keeps the crowd from stabilizing.

**Files:**
- Modify: `main/ble_devices.c` (add death/rebirth to `ble_devices_tick`)
- Test: `tools/decoy_audit/tests/test_ble_devices.py`

**Interfaces:**
- Consumes: `dev_spawn`, `s_dev`, `s_n` (Task 1); rotation loop (Task 2).
- Produces: bounded-lifetime turnover; a reborn slot has a fresh identity with no carry-over.

- [ ] **Step 1: Write the failing tests.** Append to `test_ble_devices.py`:

```python
@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class Lifecycle(unittest.TestCase):
    def slot_births(self, rows):
        by_slot = defaultdict(list)
        for r in sorted(rows, key=lambda r: (r[1], r[0])):
            if r[5] == "born": by_slot[r[1]].append(r)
        return by_slot

    def test_population_turns_over(self):
        rows = sim(21, n=16, ticks=8000, tick_ms=1000)     # ~133 min simulated
        births = self.slot_births(rows)
        total_births = sum(len(v) for v in births.values())
        self.assertGreater(total_births, 16, "no rebirths — population never turned over")

    def test_lifetime_bounded_by_role(self):
        rows = sim(22, n=24, ticks=12000, tick_ms=1000)
        births = self.slot_births(rows)
        for slot, bs in births.items():
            for a, b in zip(bs, bs[1:]):
                span = b[0] - a[0]                          # birth-to-rebirth ≈ life_ms
                role = a[4]
                cap = 720000 if role == "transient" else 5400000
                self.assertLessEqual(span, cap + 1000, f"{role} slot {slot} outlived its band: {span} ms")

    def test_no_address_resurrection(self):
        rows = sim(23, n=24, ticks=10000, tick_ms=1000)
        seen = set()
        for _, _, addr, *_ in rows:
            # an address may repeat only as consecutive same-slot events (already unique per emit);
            # here every emitted address must be globally unique (fresh 46-bit random each time)
            self.assertNotIn(addr, seen, "an address reappeared after use")
            seen.add(addr)

    def test_rebirth_is_fresh(self):
        rows = sim(24, n=24, ticks=10000, tick_ms=1000)
        births = self.slot_births(rows)
        multi = {s: bs for s, bs in births.items() if len(bs) >= 2}
        self.assertTrue(multi, "no slot was reborn to compare")
        for slot, bs in multi.items():
            addrs = [b[2] for b in bs]
            self.assertEqual(len(addrs), len(set(addrs)), f"slot {slot} reused an address on rebirth")
```

- [ ] **Step 2: Run — expect FAIL** (no death yet → no rebirths → `test_population_turns_over` fails).

Run: `python -m unittest discover -s tools\decoy_audit\tests -v`
Expected: `test_population_turns_over` FAILS on "no rebirths".

- [ ] **Step 3: Implement death/rebirth.** In `main/ble_devices.c`, add the death/rebirth pass at the **top** of `ble_devices_tick` (before the rotation loop) so a reborn device gets a correct rotation schedule the same tick:

```c
void ble_devices_tick(uint32_t now_ms)
{
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (d->alive && (now_ms - d->born_ms) >= d->life_ms) {
            dev_spawn(d, now_ms);          // dies; reborn fresh (new subtype/role/behaviour/addr/phase)
        }
    }
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (!d->alive) continue;
        if (d->atype == BLE_ATYPE_STATIC) continue;
        if ((int32_t)(now_ms - d->next_rotate_ms) >= 0) {
            make_random_addr(d->id.addr, top2_for(d->atype));
            d->next_rotate_ms = now_ms + rotate_base(d->atype);
        }
    }
}
```

- [ ] **Step 4: Run — expect PASS.**

Run: `python -m unittest discover -s tools\decoy_audit\tests -v`
Expected: all `Lifecycle` tests PASS (turnover, role-bounded lifetimes, no address resurrection, fresh rebirth), and every earlier test still PASS.

- [ ] **Step 5: Commit.**
```bash
git add main/ble_devices.c tools/decoy_audit/tests/test_ble_devices.py
git commit -m "feat(ble): death/rebirth lifecycle — role-bounded lifetimes + turnover"
```

---

## Task 4: Temporal validator + optional real-vs-synth presence-duration

Graduate a permanent temporal analyzer that scores the `--devices` output against the spec §7 bar (rotation cadence per subtype, lifetime cohort, rotation independence, no resurrection), and add the aggregate presence-duration histogram to `capture_profile.py` so the synthetic crowd can be JS-compared to a real capture (the "lifespan cohort" slice the `decoy_audit` README lists as deferred).

**Files:**
- Create: `tools/decoy_audit/analyzers/rotation_audit.py`, `tools/decoy_audit/analyzers/README.md`
- Modify: `tools/decoy_audit/capture_profile.py` (add aggregate presence-duration histogram to `profile.json`)

**Interfaces:**
- Consumes: `synth_dump --devices` stdout; optionally a `profile.json` from `capture_profile.py`.
- Produces: a PASS/FAIL temporal report for the milestone sign-off.

- [ ] **Step 1: Add the analyzer.** Create `tools/decoy_audit/analyzers/rotation_audit.py`. It reads a `--devices` capture from a file or stdin, reconstructs per-slot life-segments (delimited by `born`), and prints PASS/FAIL per check. Bands mirror `ble_devices.c`:

```python
#!/usr/bin/env python3
"""Temporal realism audit of the BLE persistent-device generator.

Feed it the output of `synth_dump --devices <seed> <n> <ticks> <tick_ms>`:
    synth_dump --devices 1 24 8000 1000 > run.txt
    python rotation_audit.py run.txt
Exit non-zero on any FAIL. Optional: `--profile profile.json` also prints the
Jensen-Shannon divergence of the synthetic vs real per-address presence-duration
histogram (the passively-observable projection of rotation + lifetime).
"""
import sys, argparse, math
from collections import defaultdict, Counter

RPA_LO, RPA_HI = 600000, 1200000            # 10-20 min band (ms)
TRANSIENT_CAP, RESIDENT_CAP = 720000, 5400000
PRESENCE_BINS = [0, 60000, 300000, 900000, 1800000, 3600000, 7200000, 10**12]  # <1,5,15,30,60,120,>120 min

def parse(fh):
    rows = []
    for ln in fh:
        p = ln.split()
        if len(p) == 9 and p[0] == "D":
            rows.append((int(p[1]), int(p[2]), p[3], p[4], p[5], p[6], int(p[7]), int(p[8])))
    return rows

def segments(rows):
    by_slot = defaultdict(list)
    for r in sorted(rows, key=lambda r: (r[1], r[0])):
        by_slot[r[1]].append(r)
    segs = []
    for evs in by_slot.values():
        cur = None
        for e in evs:
            if e[5] == "born":
                if cur: segs.append(cur)
                cur = [e]
            elif cur:
                cur.append(e)
        if cur: segs.append(cur)
    return segs

def jsd(p, q):
    s = lambda v: [x / (sum(v) or 1) for x in v]
    p, q = s(p), s(q); m = [(a + b) / 2 for a, b in zip(p, q)]
    kl = lambda a, b: sum(x * math.log2(x / y) for x, y in zip(a, b) if x > 0 and y > 0)
    return (kl(p, m) + kl(q, m)) / 2

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile", nargs="?", help="devices dump (default: stdin)")
    ap.add_argument("--profile", help="profile.json with presence_ms_bins for real-vs-synth JSD")
    a = ap.parse_args()
    rows = parse(open(a.infile) if a.infile else sys.stdin)
    if not rows:
        print("FAIL: no device events parsed"); sys.exit(1)
    segs = segments(rows)
    fails = []

    # 1. static never rotates; RPA gaps in band
    for s in segs:
        at = s[0][3]
        ts = [e[0] for e in s if e[5] in ("born", "rotate")]
        gaps = [b - a for a, b in zip(ts, ts[1:])]
        if at == "static" and any(e[5] == "rotate" for e in s):
            fails.append("static device rotated")
        if at == "rpa":
            for g in gaps:
                if not (RPA_LO - 1000 <= g <= RPA_HI + 1000):
                    fails.append(f"RPA rotation out of band: {g} ms")

    # 2. lifetime cohort bounded by role
    by_slot = defaultdict(list)
    for s in segs: by_slot[s[0][1]].append(s[0][0])
    for s in segs:
        births = sorted(by_slot[s[0][1]])
        for a2, b2 in zip(births, births[1:]):
            cap = TRANSIENT_CAP if s[0][4] == "transient" else RESIDENT_CAP
            if b2 - a2 > cap + 1000:
                fails.append(f"{s[0][4]} lifetime over cap: {b2 - a2} ms")

    # 3. rotation independence (no synchronized volley)
    live = max(e[1] for e in rows) + 1
    per_tick = Counter(e[0] for e in rows if e[5] == "rotate")
    if per_tick and max(per_tick.values()) >= max(2, live // 2):
        fails.append("synchronized rotation volley")

    # 4. no address resurrection
    seen = set()
    for _, _, addr, *_ in rows:
        if addr in seen: fails.append("address reused"); break
        seen.add(addr)

    for label, ok in [("rotation-cadence", not any("RPA rotation" in f or "static" in f for f in fails)),
                      ("lifetime-cohort", not any("lifetime" in f for f in fails)),
                      ("rotation-independence", "synchronized rotation volley" not in fails),
                      ("no-resurrection", "address reused" not in fails)]:
        print(f"{'PASS' if ok else 'FAIL'}  {label}")

    if a.profile:
        import json
        prof = json.load(open(a.profile)).get("presence_ms_bins")
        if prof:
            durs = []
            for s in segs:
                durs.append(s[-1][0] - s[0][0])
            synth = [0] * (len(PRESENCE_BINS) - 1)
            for d in durs:
                for k in range(len(PRESENCE_BINS) - 1):
                    if PRESENCE_BINS[k] <= d < PRESENCE_BINS[k+1]: synth[k] += 1; break
            print(f"presence-duration JSD vs real: {jsd(synth, prof):.4f}")

    if fails:
        print("FAILS:", "; ".join(sorted(set(fails)))); sys.exit(1)
    print("ALL TEMPORAL CHECKS PASS")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Add the presence histogram to `capture_profile.py`.** In `tools/decoy_audit/capture_profile.py`, while it already walks records and tracks each AdvA, additionally record per-address first-seen/last-seen timestamps, then emit an aggregate `presence_ms_bins` array (7 buckets matching `PRESENCE_BINS` in Step 1) into the output `profile.json`. Emit **only** the bucket counts — never an address, timestamp pair, name, or AD byte. If the capture has no usable timestamps, emit `presence_ms_bins` as seven zeros and note it in stderr.

- [ ] **Step 3: Add the analyzer README.** Create `tools/decoy_audit/analyzers/README.md` documenting: the four temporal checks and their PASS conditions verbatim from spec §7; the exact `synth_dump --devices` command; the optional `--profile` JSD comparison; and a bold reminder that captures and their profiles stay in the gitignored `private/` and are never committed. State plainly that the analyzer scores the synthetic generator's temporal behaviour and the presence-duration JSD; it does not measure the physical layer (RSSI/AoA), consistent with the spec's honest ceiling.

- [ ] **Step 4: Smoke-run the analyzer on a fresh `--devices` run.**

Run:
```
tools\decoy_audit\synth_dump.exe --devices 1 24 8000 1000 > %TEMP%\dev.txt
python tools\decoy_audit\analyzers\rotation_audit.py %TEMP%\dev.txt
```
Expected: `PASS` on all four checks and `ALL TEMPORAL CHECKS PASS` (exit 0). This is the same behaviour the Task-2/3 unit tests assert, now packaged as the permanent milestone validator.

- [ ] **Step 5: Commit.**
```bash
git add tools/decoy_audit/analyzers/rotation_audit.py tools/decoy_audit/analyzers/README.md tools/decoy_audit/capture_profile.py
git commit -m "test(ble): temporal validator (rotation cadence / lifetime cohort) + presence-duration profile"
```

---

## Task 5: Presenter integration — churn sources from ble_devices + on-air

Refactor `churn.c` from an identity-lifecycle owner into a presenter over `ble_devices`, wire `ble_devices_init` into startup, build/flash both boards, and validate on-air.

**Files:**
- Modify: `main/churn.c`, `main/churn.h`, `main/simulacra_main.c`, `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `ble_devices_init`, `ble_devices_tick`, `ble_devices_count`, `ble_devices_at` (Tasks 1–3); `churn_adv_apply` (unchanged); `generate_active_target` (unchanged).
- Produces: live BLE advertising driven by the persistent-device population, re-applying an instance on address rotation.

- [ ] **Step 1: Add the source to CMake.** In `main/CMakeLists.txt`, add `"ble_devices.c"` to the `SRCS` list (place it next to `"churn.c"`).

- [ ] **Step 2: Refactor `churn.c` into a presenter.** Replace the identity-lifecycle internals with a map from `ble_devices` onto the 4 hardware instances, re-applying on address change. Replace the top-of-file active-set state and `churn_init`/`churn_tick`/`churn_active_count`/`churn_active_at` with:

```c
// Presenter state: which device index occupies each hardware instance, and the address last
// applied there (so a rotation — same device, new address — triggers a single re-apply).
static int      s_occ_idx[CHURN_HW_INSTANCES];
static uint8_t  s_occ_addr[CHURN_HW_INSTANCES][6];
static uint32_t s_phase;
static uint32_t s_last_slice_ms;
static churn_apply_fn s_apply;
static bool     s_paused;

void churn_init(uint32_t now_ms)
{
    s_phase = 0; s_last_slice_ms = now_ms;
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) { s_occ_idx[i] = -1; memset(s_occ_addr[i], 0, 6); }
}

void churn_tick(uint32_t now_ms)
{
    if (s_paused) return;                          // hold the current on-air set
    ble_devices_tick(now_ms);                      // advance death/rebirth + rotation
    if (now_ms - s_last_slice_ms < CHURN_SLICE_MS) return;
    s_last_slice_ms = now_ms; s_phase++;

    int pop = ble_devices_count();
    if (pop <= 0) return;
    for (int i = 0; i < CHURN_HW_INSTANCES; i++) {
        int idx;
        if (pop <= CHURN_HW_INSTANCES) {
            if (i >= pop) continue;                // fewer devices than radios
            idx = i;
        } else {
            idx = (int)((s_phase * CHURN_HW_INSTANCES + i) % pop);
        }
        const ble_device_t *d = ble_devices_at(idx);
        if (!d) continue;
        if (s_occ_idx[i] != idx || memcmp(s_occ_addr[i], d->id.addr, 6) != 0) {
            s_occ_idx[i] = idx;
            memcpy(s_occ_addr[i], d->id.addr, 6);
            if (s_apply) s_apply((uint8_t)i, &d->id);   // (re)apply this device on instance i
        }
    }
}

size_t churn_active_count(void) { return (size_t)ble_devices_count(); }

const identity_t *churn_active_at(size_t slot)
{
    const ble_device_t *d = ble_devices_at((int)slot);
    return d ? &d->id : 0;
}
```
Add `#include "ble_devices.h"` and `#include <string.h>` at the top of `churn.c`. Delete the now-unused active-set lifecycle: `s_active[]`, `s_occupant[]`, `s_active_target`, the `roster_promote_candidate` calls, `dwell_ms`, and the `#include "roster.h"` promotion usage. Keep `churn_set_apply` and `churn_set_paused`/`churn_paused` as they are.

- [ ] **Step 3: Retain the deprecated tuning setters so dependents still build.** The dwell/cooldown/accel/active-target setters are no longer the lifecycle authority (lifetime now lives in `ble_devices`), but `settings.c`/`webui.c`/`coexist.c` still call them. In `main/churn.c`, keep these as retained no-ops that store their value without driving anything, so the build and the webui controls stay intact:

```c
void    churn_set_active_target(uint8_t n) { (void)n; }   // population size owned by ble_devices_init (Milestone A)
uint8_t churn_active_target(void) { return (uint8_t)ble_devices_count(); }
void    churn_set_accel(float mult) { (void)mult; }
void    churn_set_dwell_ms(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }
void    churn_set_cooldown_ms(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }
void    churn_get_dwell_ms(uint32_t *lo, uint32_t *hi) { if (lo) *lo = 0; if (hi) *hi = 0; }
void    churn_get_cooldown_ms(uint32_t *lo, uint32_t *hi) { if (lo) *lo = 0; if (hi) *hi = 0; }
```
In `main/churn.h`, add a one-line note above these declarations: `// Milestone A: lifetime/rotation owned by ble_devices; these tuning setters are retained (inert) for API compatibility.` Do not change the declarations themselves.

- [ ] **Step 4: Initialize `ble_devices` at startup.** In `main/simulacra_main.c`, at the block that currently does `roster_init(); ... churn_set_apply(churn_adv_apply); churn_init(...)` (around lines 137–148), insert the device-population init after `roster_init()` and the active-target computation, before `churn_init`:

```c
    int ndev = (rf_model_load_nvs(&m) == 0 && m.total_obs >= GEN_MIN_OBS)
                 ? (int)generate_active_target(&m) : 12;      // population size; ble_devices clamps to max
    ble_devices_init(ndev, (uint32_t)(esp_timer_get_time() / 1000));
```
Keep the existing `churn_set_apply(churn_adv_apply); churn_init(...)` calls that follow. (If `m` is already loaded in that block, reuse it rather than loading twice — match the surrounding code.) Add `#include "ble_devices.h"` to `simulacra_main.c` if not already pulled in via `churn.h`.

- [ ] **Step 5: Confirm nothing else depends on the removed internals.**

Run: `grep -rn "roster_promote_candidate\|churn_active_at\|s_active\b" main/`
Expected: `roster_promote_candidate` now has no callers (its definition in `roster.c` may remain, unused — leave it); `churn_active_at` is called only by `coexist.c` and returns a valid `identity_t*` as before; no stray `s_active` references remain in `churn.c`.

- [ ] **Step 6: Host regression.** Rebuild the host harness and run the full host suite (the pure core is untouched by this glue task, so everything stays green):

Run:
```
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c learn_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c /Fe:synth_dump.exe
python -m unittest discover -s tools\decoy_audit\tests -v
```
Expected: all `test_ble_devices.py` and `test_synth_dump.py` tests PASS.

- [ ] **Step 7: Build + flash both boards and smoke-check rotation on serial.** Build/flash the C5 (COM12 or COM16) and the C6 (COM13) via the build-flash-read skill (which now points at the correct `simulacra` tree). Read past the config window (≥42 s on the C5).

Run (C5 example):
```
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Port COM16 -Do all -ReadSeconds 46 -Grep 'coexist|churn|adv|decoy alive'
```
Expected: both boards flash `Hash of data verified`; serial shows the BLE decoy running (coexist BLE-only or combined) with no `churn_adv configure/set_addr rc` error spam — i.e. the controller accepts the rotation re-applies. Fleet stays healthy (`fleet: enrolled` if provisioned).

- [ ] **Step 8: On-air capture + audits.** Take a BLE capture near the fleet (nRF/Kismet → pcap saved to `private/`). Run the snapshot scorecard (must not regress) and record a presence-duration sanity:

Run:
```
pwsh tools\decoy_audit\run.ps1 -Capture <private\CAP.pcap> -Rebuild
python tools\decoy_audit\capture_profile.py <private\CAP.pcap> <private\p.json> <private\m.seed>
```
Expected: the snapshot headline stays low (interval/vendor/address-type mix **not** regressed vs the ~0.004 baseline), and `p.json` now carries a `presence_ms_bins` histogram whose mass spans short (transient) through long (resident) buckets rather than a single dwell band. Record results in `private/PROBE-ANTIFINGERPRINT-STANCE.md` (BLE section).

- [ ] **Step 9: Commit.**
```bash
git add main/churn.c main/churn.h main/simulacra_main.c main/CMakeLists.txt
git commit -m "feat(ble): churn presents the persistent-device population + rotation re-apply"
```

---

## Task 6: Finish the branch

- [ ] **Step 1:** Confirm the host suite is green (`python -m unittest discover -s tools\decoy_audit\tests -v`), the temporal analyzer passes (`rotation_audit.py`), and both boards are flashed clean.
- [ ] **Step 2:** Record the milestone outcome (rotation on-air, snapshot non-regression, presence-duration spread) in `private/PROBE-ANTIFINGERPRINT-STANCE.md` and add/refresh a memory note for the BLE RPA-rotation milestone. Note that Milestone B (persona clusters + ecosystem profiles) is the next step and builds on this.
- [ ] **Step 3:** Use superpowers:finishing-a-development-branch to verify tests, present merge/PR options, and complete the branch.

---

## Self-Review

- **Spec coverage:** §1 tells (no rotation; lifetime monoculture) → Tasks 2 (rotation), 3 (role-split lifetime). §2 goals (RPA rotate / NRPA faster / static none / 70-30 split) → Tasks 1–3. §4 architecture (pure `ble_devices` core + reuse content/apply + churn as presenter) → Tasks 1–3 (core), 5 (presenter). §5 model + timing bands → Tasks 1–3 constants and tests. §6 rotation realism (behaviour preserved, independent phase, subtype↔policy, no BLE seq gate) → Task 2 tests + Step-3 impl. §7 validation bar (rotation cadence, lifetime cohort, independence, continuity, no resurrection) → Tasks 2–4. §8 testing (host `--devices` dumper, cadence/continuity/phase/lifetime/rebirth) → Tasks 1–3; on-air smoke + capture → Task 5. §9 non-goals honored (no clusters, no ecosystem profiles, no physical claims, no raised concurrency guarantee, no capture-driven params, no inter-board coordination). §10 constraints + §3/§11 honest ceiling & risks → Global Constraints and Task 5 Step 3 (retained inert setters) / Step 8 (non-regression). Covered.
- **Placeholder scan:** none — every step carries concrete code, exact commands, and expected output. Task 4 Step 2 (capture_profile edit) and Task 5 Step 4 (simulacra_main insertion point) describe an edit to existing code precisely rather than pasting an unseen full file, which is the correct granularity for a localized modification.
- **Type consistency:** `ble_device_t` (embeds `identity_t id`), `ble_atype_t {BLE_ATYPE_STATIC/RPA/NRPA}`, `ble_role_t {BLE_ROLE_TRANSIENT/RESIDENT}`, and the API `ble_devices_init(int,uint32_t)` / `ble_devices_tick(uint32_t)` / `ble_devices_count(void)` / `ble_devices_at(int)` are used identically in the header (Task 1), dumper (Task 1), impl (Tasks 1–3), and presenter (Task 5). `top2_for`/`rotate_base`/`dev_spawn` are file-local helpers defined in Task 1 and reused in Tasks 2–3. `churn_active_at` keeps returning `const identity_t *` (Task 5) so `coexist.c` is unchanged. The dumper row format `D t slot addr atype role event company itvl` (9 fields) matches the parser in `test_ble_devices.py` and `rotation_audit.py`.
- **Ordering risk:** Task 3's death/rebirth pass is placed before the rotation pass in `ble_devices_tick` so a reborn device is scheduled correctly the same tick — called out in Task 3 Step 3. The Task-2 rotation tests remain valid after Task 3 because rebirth only adds `born` events; segment reconstruction is delimited by `born`, so rotation-in-band assertions stay scoped to a single life-segment.
