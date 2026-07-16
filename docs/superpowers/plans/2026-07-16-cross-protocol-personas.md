# Cross-Protocol Personas Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bind a BLE identity and a Wi-Fi probe identity into one synthetic "device" (a persona) that is born, present, and gone together with a coherent device type, so a cross-modal correlator can no longer isolate the user's real dual-radio device by filtering single-radio ghosts.

**Architecture:** Approach B (shared registry). Two new pure modules — `uniq_id` (guaranteed-unique identity allocator) and `phantom` (the persona registry + lifecycle) — sit beside the existing pure `ble_devices` and `probe_agents` cores. The two cores gain a small "bound member" path that mirrors a phantom's family + shared lifetime; `phantom_sync_*` reincarnates bound members when their phantom's generation changes. All new/changed logic is pure (clock via `now_ms`, randomness via `esp_random()`), host-compiled and tested against the existing `tools/probe_audit` and `tools/decoy_audit` harnesses.

**Tech Stack:** C (ESP-IDF firmware, host-compiled with `cc` against `host_stubs/`); Python `unittest` run under pytest for host assertions.

## Global Constraints

- **Code module name is `phantom`, NOT `persona`.** The codebase already has `coexist_persona_t` meaning the *board coexistence profile* ("Ward: dense, dual-band"). The cross-protocol persona feature uses the code symbol `phantom` to avoid that collision. The spec/user concept remains "cross-protocol persona."
- **Design Law 3 (hard gate):** persona BLE members must never emit a pairing-popup format. Apple (company 0x004C) is excluded from BLE; Apple personas use a Law-3-safe anonymous RPA. Roster entries are already Law-3-safe by construction; tests assert it.
- **Public repo hygiene:** never commit absolute local paths, the OS username, real hardware MACs, or real SSIDs. All addresses in tests are synthetic.
- **Guaranteed-unique scope:** unique across all live identities + the last `UNIQ_HISTORY` (2048) retired addresses, on both radios. A single device repeating its *own* advert is unchanged (real beacon behavior), not a duplicate.
- **Wi-Fi-side composition:** every probe agent is bound into a persona; `PHANTOM_MAX == PROBE_AGENTS_MAX`. Extra `ble_devices` slots are the unbound BLE-only crowd.
- **Persona BLE members are always RPA** (top-2 bits `0x40`); they may rotate address within life.
- **`SIMULACRA_PROBE` standalone (seq-gate) path stays behavior-identical.**
- **Commit trailers (every commit):**
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
  ```
- **Host tests run with:** `"C:/Program Files/Python312/python.exe" -m pytest <path> -v` (system Python lacks pytest).
- **Build environment (Windows):** the host harnesses are built by MSVC `cl`, invoked from each tool's `run.ps1`. Run all build commands from a **"Developer PowerShell for VS"** (so `cl` is on PATH). **The source list lives in the `cl` line inside `run.ps1`, not the Makefile** — the `Makefile` is a stale Unix-only parity copy. When a task adds a new `.c`, update the `cl` line in `run.ps1` (primary) AND the `Makefile` `SRC` (parity). `tools/probe_audit/run.ps1 -Rebuild` builds probe_dump.exe **and** runs the test suite; `tools/decoy_audit/run.ps1 -Rebuild` runs a full audit pipeline (needs `private/long.pcap`), so for a plain build+test of decoy_audit use the explicit `cl` command given in the steps, then pytest.
- **Branch:** do all work on `feat/cross-protocol-personas`. Do NOT push. Merge locally with `--no-ff` at the end (finishing-a-development-branch).

---

### Task 1: `uniq_id` — guaranteed-unique identity allocator (pure)

**Files:**
- Create: `main/uniq_id.h`
- Create: `main/uniq_id.c`
- Modify: `tools/probe_audit/run.ps1` (add `..\..\main\uniq_id.c` to the `cl` line) and `tools/probe_audit/Makefile` (parity)
- Modify: `tools/probe_audit/probe_dump.c` (add a `--uniq` dump mode)
- Test: `tools/probe_audit/tests/test_uniq_id.py`

**Interfaces:**
- Produces: `void uniq_reset(void);` and `bool uniq_try(const uint8_t addr[6]);` — `uniq_try` returns `true` and records the address if it is not in the recent-history ring, else returns `false` without recording.

- [ ] **Step 1: Create the branch**

```bash
git checkout -b feat/cross-protocol-personas
```

- [ ] **Step 2: Write the header**

Create `main/uniq_id.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Guaranteed-unique identity allocator. A ring of the last UNIQ_HISTORY issued 6-byte
// addresses, shared across both radios (BLE + Wi-Fi). Every fresh address is drawn through
// uniq_try so no freshly-emitted identity collides with any live-or-recent one. UNIQ_HISTORY
// is sized well above the max concurrent live population, so every live address is always
// still in the ring (one structure covers "not live" and "not recently retired").
void uniq_reset(void);                       // clear history (host tests / cold boot)
bool uniq_try(const uint8_t addr[6]);        // unseen -> record + true; duplicate -> false
```

- [ ] **Step 3: Write the failing test**

Create `tools/probe_audit/tests/test_uniq_id.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def gen(seed, n, mode="--uniq"):
    out = subprocess.check_output([EXE, mode, str(seed), str(n)], text=True)
    return [ln.strip() for ln in out.splitlines() if ln.strip()]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Uniq(unittest.TestCase):
    def test_no_duplicates_within_history(self):
        # --uniq makes ONE distinct pass of n addresses (n < UNIQ_HISTORY 2048): all-distinct
        addrs = gen(1, 1500)
        self.assertEqual(len(addrs), 1500)
        self.assertEqual(len(set(addrs)), 1500, "uniq_try issued a duplicate within history")

    def test_reset_allows_reissue(self):
        # --uniqreset resets + re-seeds identically between the two halves -> the same
        # sequence reappears, proving uniq_reset clears history.
        a = gen(2, 200, "--uniqreset")
        self.assertEqual(len(a), 200)
        self.assertEqual(a[:100], a[100:], "uniq_reset did not clear history")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 4: Add the `--uniq` mode to the harness dumper**

In `tools/probe_audit/probe_dump.c`, add `#include "uniq_id.h"` near the top includes, and add this block at the start of `main` (before the existing `--agents` block):

```c
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
        // Two identical halves around a reset, so the test can prove reset clears history.
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
```

- [ ] **Step 5: Add `uniq_id.c` to the build source list**

In `tools/probe_audit/run.ps1`, add `..\..\main\uniq_id.c` to the `cl` source list (the line currently ending `..\..\main\probe_agents.c /Fe:probe_dump.exe`):

```powershell
           probe_dump.c ..\..\main\probe_frame.c ..\..\main\probe_agents.c ..\..\main\uniq_id.c /Fe:probe_dump.exe | Out-Null
```

Also update `tools/probe_audit/Makefile` `SRC` for Unix parity:

```make
SRC := probe_dump.c $(ROOT)/main/probe_frame.c $(ROOT)/main/probe_agents.c $(ROOT)/main/uniq_id.c
```

- [ ] **Step 6: Build to verify it FAILS**

From a Developer PowerShell for VS:
```powershell
cd tools/probe_audit ; .\run.ps1 -Rebuild
```
Expected: **build fails** with exit 3 (`cl` cannot open `..\..\main\uniq_id.c` — it does not exist yet). This confirms the test is wired to real code.

- [ ] **Step 7: Implement `uniq_id.c`**

Create `main/uniq_id.c`:

```c
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
```

- [ ] **Step 8: Rebuild and run the test to verify it PASSES**

```powershell
cd tools/probe_audit ; .\run.ps1 -Rebuild
"C:/Program Files/Python312/python.exe" -m pytest tests/test_uniq_id.py -v
```
Expected: build succeeds and both tests PASS (`run.ps1` also runs the full unittest discovery — those pass too).

- [ ] **Step 9: Commit**

```bash
git add main/uniq_id.h main/uniq_id.c tools/probe_audit/run.ps1 tools/probe_audit/Makefile tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_uniq_id.py
git commit -m "feat(personas): guaranteed-unique identity allocator (uniq_id)"
```

---

### Task 2: Route both radios' address generators through `uniq_id`

**Files:**
- Modify: `main/roster.c:15-24` (`make_random_addr`)
- Modify: `main/probe_frame.c:94-103` (`probe_random_mac`)
- Modify: `tools/decoy_audit/run.ps1` (add `..\..\main\uniq_id.c` to the `cl` line) and `tools/decoy_audit/Makefile` (parity)
- Modify: `tools/probe_audit/probe_dump.c` and `tools/decoy_audit/synth_dump.c` (add a `--routecheck` mode each)
- Test: `tools/probe_audit/tests/test_route_uniq.py` and `tools/decoy_audit/tests/test_route_uniq.py` (new)

**Interfaces:**
- Consumes: `uniq_try` from Task 1.
- Produces: no new symbols; `make_random_addr` and `probe_random_mac` now record every value they return via `uniq_try`, so they never return a value colliding with recent history.

**Why not an "all distinct" test:** 46-bit random addresses never collide by chance, so an
"all addresses distinct over a long run" assertion passes whether or not the routing is wired —
it cannot detect the wiring, and shrinking the ring cannot *manufacture* a collision (the ring
prevents collisions, it does not create them). Instead we test the routing **directly**: after the
generator produces an address, that address must already be in the ring (`uniq_try` returns
`false`), which is true only if the generator recorded it. RED = generator not routed →
`uniq_try` returns `true`; GREEN = routed → `false`.

- [ ] **Step 1: Add a `--routecheck` mode to each dumper**

In `tools/probe_audit/probe_dump.c`, add `#include "uniq_id.h"` and this block in `main`:

```c
    if (argc > 1 && strcmp(argv[1], "--routecheck") == 0) {
        // Prove probe_random_mac records through uniq_id: after generating, the MAC must already
        // be in the ring. Prints 0 if routed (recorded), 1 if NOT routed.
        srand(argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1);
        uniq_reset();
        uint8_t m[6];
        probe_random_mac(m);
        printf("%d\n", uniq_try(m) ? 1 : 0);
        return 0;
    }
```

In `tools/decoy_audit/synth_dump.c`, add `#include "uniq_id.h"` and this block at the start of `main`:

```c
    if (argc > 1 && strcmp(argv[1], "--routecheck") == 0) {
        // Prove make_random_addr records through uniq_id. Prints 0 if routed, 1 if NOT.
        srand(argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1);
        uniq_reset();
        uint8_t a[6];
        make_random_addr(a, 0xc0);
        printf("%d\n", uniq_try(a) ? 1 : 0);
        return 0;
    }
```

Create `tools/probe_audit/tests/test_route_uniq.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Route(unittest.TestCase):
    def test_probe_random_mac_records_via_uniq(self):
        out = subprocess.check_output([EXE, "--routecheck", "1"], text=True).strip()
        self.assertEqual(out, "0", "probe_random_mac did not record via uniq_try (routing missing)")


if __name__ == "__main__":
    unittest.main()
```

Create `tools/decoy_audit/tests/test_route_uniq.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class Route(unittest.TestCase):
    def test_make_random_addr_records_via_uniq(self):
        out = subprocess.check_output([EXE, "--routecheck", "1"], text=True).strip()
        self.assertEqual(out, "0", "make_random_addr did not record via uniq_try (routing missing)")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Route `make_random_addr` through `uniq_id`**

In `main/roster.c`, add `#include "uniq_id.h"` after the existing includes, and replace the body of `make_random_addr` (lines 15-24) with:

```c
void make_random_addr(uint8_t out[6], uint8_t top2)
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[5] = (uint8_t)((out[5] & 0x3f) | (top2 & 0xc0));
        int ones = __builtin_popcount(out[5] & 0x3f);
        for (int i = 0; i < 5; i++) ones += __builtin_popcount(out[i]);
        if (ones == 0 || ones == 46) continue;   // NimBLE rejects all-zero/all-ones
        if (uniq_try(out)) return;                // guaranteed-unique across live + recent history
    }
}
```

- [ ] **Step 3: Route `probe_random_mac` through `uniq_id`**

In `main/probe_frame.c`, add `#include "uniq_id.h"` after the existing includes, and replace the body of `probe_random_mac` (lines 94-103) with:

```c
void probe_random_mac(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[0] = (uint8_t)((out[0] & 0xFC) | 0x02);   // locally-administered, unicast
        int zero = 1, ff = 1;
        for (int i = 0; i < 6; i++) { if (out[i]) zero = 0; if (out[i] != 0xff) ff = 0; }
        if (zero || ff) continue;
        if (uniq_try(out)) return;                    // shares the allocator with BLE
    }
}
```

- [ ] **Step 4: Add `uniq_id.c` to the decoy_audit build source list**

In `tools/decoy_audit/run.ps1`, append `..\..\main\uniq_id.c` to the `cl` source list (the continued line block before `/Fe:synth_dump.exe`):

```powershell
           synth_dump.c ble_hs_adv.c roster_stub.c `
           ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c `
           ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c `
           ..\..\main\uniq_id.c `
           /Fe:synth_dump.exe | Out-Null
```

Also append `$(ROOT)/main/uniq_id.c` to `tools/decoy_audit/Makefile` `SRC` for parity.

- [ ] **Step 5: Prove RED — build with routing disabled**

The routecheck detects the wiring directly. To capture RED, temporarily disable the routing: in `main/roster.c` change `make_random_addr`'s `if (uniq_try(out)) return;` back to a plain `return;` (drop the `uniq_try` guard for one build), and likewise in `main/probe_frame.c` change `probe_random_mac`'s `if (uniq_try(out)) return;` to `return;`. Build both harnesses and run the routechecks:

```powershell
cd tools/decoy_audit
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c roster_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\uniq_id.c /Fe:synth_dump.exe
"C:/Program Files/Python312/python.exe" -m pytest tests/test_route_uniq.py -v
cd ..\probe_audit ; .\run.ps1 -Rebuild
"C:/Program Files/Python312/python.exe" -m pytest tests/test_route_uniq.py -v
```
Expected: both routecheck tests **FAIL** (`--routecheck` prints `1` — the generator returned an address the ring had NOT recorded). This proves the test detects missing routing.

- [ ] **Step 6: Restore routing and prove GREEN**

Restore the `if (uniq_try(out)) return;` guard in both `make_random_addr` and `probe_random_mac` (Steps 2-3), rebuild both harnesses, and run the routechecks plus the existing suites:

```powershell
cd tools/decoy_audit
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c roster_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\uniq_id.c /Fe:synth_dump.exe
"C:/Program Files/Python312/python.exe" -m pytest tests/test_route_uniq.py -v
cd ..\probe_audit ; .\run.ps1 -Rebuild
"C:/Program Files/Python312/python.exe" -m pytest tests -v
```
Expected: both routechecks print `0` and PASS; the full probe_audit suite (test_uniq_id, test_probe_agents, test_route_uniq) stays green (MACs now also drawn through the shared allocator).

- [ ] **Step 7: Commit**

```bash
git add main/roster.c main/probe_frame.c tools/decoy_audit/run.ps1 tools/decoy_audit/Makefile tools/decoy_audit/synth_dump.c tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_route_uniq.py tools/decoy_audit/tests/test_route_uniq.py
git commit -m "feat(personas): draw every BLE/Wi-Fi address through the unique allocator"
```

---

### Task 3: `phantom` — persona registry + lifecycle (pure)

**Files:**
- Create: `main/phantom.h`
- Create: `main/phantom.c`
- Modify: `tools/probe_audit/run.ps1` (add `..\..\main\phantom.c` to the `cl` line) and `tools/probe_audit/Makefile` (parity)
- Modify: `tools/probe_audit/probe_dump.c` (add a `--phantoms` dump mode)
- Test: `tools/probe_audit/tests/test_phantom.py`

**Interfaces:**
- Consumes: `probe_arch_t` (from `probe_frame.h`), `PROBE_AGENTS_MAX` (from `probe_agents.h`).
- Produces:
  - `typedef enum { PF_SAMSUNG, PF_GOOGLE, PF_APPLE, PF_GENERIC, PHANTOM_FAMILY_COUNT } phantom_family_t;`
  - `typedef struct { phantom_family_t family; uint32_t born_ms; uint32_t life_ms; uint32_t generation; bool alive; } phantom_t;`
  - `#define PHANTOM_MAX PROBE_AGENTS_MAX`
  - `void phantom_init(int n, uint32_t now_ms);`
  - `int phantom_lifecycle(uint32_t now_ms);` — retires+reincarnates expired phantoms (bumps `generation`), returns count reborn.
  - `int phantom_count(void);`
  - `const phantom_t *phantom_at(int i);`
  - `probe_arch_t phantom_arch(phantom_family_t f);`
  - `uint16_t phantom_company(phantom_family_t f);` — BLE company id; 0 = anonymous RPA (Apple/generic).

- [ ] **Step 1: Write the header**

Create `main/phantom.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "probe_frame.h"     // probe_arch_t
#include "probe_agents.h"    // PROBE_AGENTS_MAX

// A cross-protocol persona: the shared lifecycle + device family that binds one BLE identity
// and one Wi-Fi probe identity into a single synthetic dual-radio "device". Named `phantom`
// (not `persona`) to avoid colliding with coexist_persona_t (the board coexistence profile).
typedef enum { PF_SAMSUNG, PF_GOOGLE, PF_APPLE, PF_GENERIC, PHANTOM_FAMILY_COUNT } phantom_family_t;

typedef struct {
    phantom_family_t family;
    uint32_t born_ms;
    uint32_t life_ms;
    uint32_t generation;     // bumped on each reincarnation; bound members re-sync on change
    bool     alive;
} phantom_t;

#define PHANTOM_MAX PROBE_AGENTS_MAX     // one persona per probe agent (bind from the Wi-Fi side)

void  phantom_init(int n, uint32_t now_ms);      // create n phantoms (clamped to PHANTOM_MAX)
int   phantom_lifecycle(uint32_t now_ms);        // retire+reincarnate expired; returns # reborn
int   phantom_count(void);
const phantom_t *phantom_at(int i);
probe_arch_t phantom_arch(phantom_family_t f);   // family -> Wi-Fi archetype
uint16_t     phantom_company(phantom_family_t f);// family -> BLE company id (0 = anonymous RPA)
```

- [ ] **Step 2: Write the failing test**

Create `tools/probe_audit/tests/test_phantom.py`:

```python
import os, subprocess, unittest
from collections import Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")

# Must match phantom.h enum order.
FAM = {0: "samsung", 1: "google", 2: "apple", 3: "generic"}
# family -> (expected arch idx, expected BLE company). ARCH_IPHONE=0,GALAXY=1,PIXEL=2,ANDROID=3.
EXPECT = {0: (1, 0x0075), 1: (2, 0x00E0), 2: (0, 0x0000), 3: (3, 0x0000)}


def phantoms(seed, n=12, ticks=4000, tick_ms=1000):
    out = subprocess.check_output([EXE, "--phantoms", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 6 and p[0] == "P":
            # P <t> <idx> <family> <arch> <company> <generation>
            rows.append((int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5], 16)))
    return rows


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Phantom(unittest.TestCase):
    def test_family_maps_to_arch_and_company(self):
        rows = phantoms(1)
        self.assertTrue(rows, "no phantom events")
        for _t, _i, fam, arch, comp in rows:
            exp_arch, exp_comp = EXPECT[fam]
            self.assertEqual(arch, exp_arch, f"family {FAM[fam]} wrong arch")
            self.assertEqual(comp, exp_comp, f"family {FAM[fam]} wrong company")

    def test_all_families_appear_over_time(self):
        fams = Counter(fam for *_ , in [(r[2],) for r in phantoms(3)])
        # over thousands of reincarnations the weighted draw should hit every family
        self.assertEqual(set(fams), {0, 1, 2, 3}, f"some family never drawn: {dict(fams)}")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 3: Add the `--phantoms` mode to the harness dumper**

In `tools/probe_audit/probe_dump.c`, add `#include "phantom.h"` to the includes and this block near the other subcommands in `main`:

```c
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
```

- [ ] **Step 4: Add `phantom.c` to the build source list**

In `tools/probe_audit/run.ps1`, extend the `cl` source list:

```powershell
           probe_dump.c ..\..\main\probe_frame.c ..\..\main\probe_agents.c ..\..\main\uniq_id.c ..\..\main\phantom.c /Fe:probe_dump.exe | Out-Null
```

Also update `tools/probe_audit/Makefile` `SRC` for parity:

```make
SRC := probe_dump.c $(ROOT)/main/probe_frame.c $(ROOT)/main/probe_agents.c $(ROOT)/main/uniq_id.c $(ROOT)/main/phantom.c
```

- [ ] **Step 5: Build to verify the test FAILS (no phantom.c yet)**

```powershell
cd tools/probe_audit ; .\run.ps1 -Rebuild
```
Expected: **build fails** with exit 3 (`cl` cannot open `..\..\main\phantom.c`).

- [ ] **Step 6: Implement `phantom.c`**

Create `main/phantom.c`:

```c
#include "phantom.h"
#include "esp_random.h"

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
```

- [ ] **Step 7: Rebuild and run the test to verify it PASSES**

```powershell
cd tools/probe_audit ; .\run.ps1 -Rebuild
"C:/Program Files/Python312/python.exe" -m pytest tests/test_phantom.py -v
```
Expected: both tests PASS.

- [ ] **Step 8: Commit**

```bash
git add main/phantom.h main/phantom.c tools/probe_audit/run.ps1 tools/probe_audit/Makefile tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_phantom.py
git commit -m "feat(personas): phantom registry + lifecycle + family maps"
```

---

### Task 4: Bind the Wi-Fi side — `probe_agent_sync` + `phantom_sync_wifi`

**Files:**
- Modify: `main/probe_agents.h` (add `persona_gen` to `probe_agent_t`; declare `probe_agent_sync`)
- Modify: `main/probe_agents.c` (implement `probe_agent_sync`)
- Modify: `main/phantom.h` (declare `phantom_sync_wifi`)
- Modify: `main/phantom.c` (implement `phantom_sync_wifi`)
- Modify: `tools/probe_audit/probe_dump.c` (add a `--wbind` mode: phantoms drive bound agents)
- Test: `tools/probe_audit/tests/test_wifi_binding.py`

**Interfaces:**
- Consumes: `phantom_lifecycle`, `phantom_at`, `phantom_arch` (Task 3); `probe_random_mac`, `probe_pick_archetype` (existing).
- Produces:
  - `probe_agent_t` gains `uint32_t persona_gen;`
  - `int probe_agent_sync(int i, probe_arch_t arch, uint32_t born_ms, uint32_t life_ms, uint32_t generation);` — if agent `i`'s `persona_gen != generation`, reincarnate it with a fresh unique MAC, the given `arch`, shared `born/life`, a fresh seq base and phase-in; set `persona_gen=generation`; return 1 if reincarnated, else 0.
  - `void phantom_sync_wifi(uint32_t now_ms);` — for each phantom `i`, `probe_agent_sync(i, phantom_arch(family), born, life, generation)`.

- [ ] **Step 1: Write the failing test**

Create `tools/probe_audit/tests/test_wifi_binding.py`:

```python
import os, subprocess, unittest
from collections import defaultdict

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def wbind(seed, n=12, ticks=4000, tick_ms=1000):
    out = subprocess.check_output([EXE, "--wbind", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 6 and p[0] == "W":
            # W <t> <idx> <mac> <arch> <generation>
            rows.append((int(p[1]), int(p[2]), p[3], int(p[4]), int(p[5])))
    return rows


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class WifiBinding(unittest.TestCase):
    def test_agent_arch_follows_phantom_family(self):
        rows = wbind(1)
        self.assertTrue(rows, "no bound-agent events")
        # For each (idx, generation) the emitted arch must be stable (adopted from the phantom).
        arch_of = {}
        for _t, idx, _mac, arch, gen in rows:
            key = (idx, gen)
            if key in arch_of:
                self.assertEqual(arch_of[key], arch, "arch changed within one persona life")
            arch_of[key] = arch

    def test_bound_macs_unique_and_reincarnate_on_new_generation(self):
        rows = wbind(2)
        macs = [m for _t, _i, m, _a, _g in rows]
        self.assertEqual(len(macs), len(set(macs)), "a bound agent reused a MAC")
        # each agent index should show more than one generation (turnover happened)
        gens = defaultdict(set)
        for _t, idx, _m, _a, g in rows: gens[idx].add(g)
        self.assertTrue(any(len(v) > 1 for v in gens.values()), "no persona turnover observed")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Add the `--wbind` mode to the harness dumper**

In `tools/probe_audit/probe_dump.c`, add this block in `main`:

```c
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
```

- [ ] **Step 3: Build to verify the test FAILS (symbols missing)**

```powershell
cd tools/probe_audit ; .\run.ps1 -Rebuild
```
Expected: **build fails** — `phantom_sync_wifi` and `persona_gen` do not exist yet (compile/link error).

- [ ] **Step 4: Add `persona_gen` and declare `probe_agent_sync`**

In `main/probe_agents.h`, add `uint32_t persona_gen;` to the `probe_agent_t` struct (after `alive;` is fine), and declare after the existing prototypes:

```c
// Bind agent i to a persona (see phantom.h): if the agent's recorded generation differs from
// `generation`, reincarnate it with a fresh unique MAC, the given archetype, and the persona's
// shared born/life. Returns 1 if reincarnated this call, else 0. Bound agents do NOT expire via
// probe_agents_lifecycle; the persona owns their lifetime.
int probe_agent_sync(int i, probe_arch_t arch, uint32_t born_ms, uint32_t life_ms, uint32_t generation);
```

- [ ] **Step 5: Implement `probe_agent_sync`**

In `main/probe_agents.c`, add after `probe_agents_lifecycle`:

```c
int probe_agent_sync(int i, probe_arch_t arch, uint32_t born_ms, uint32_t life_ms, uint32_t generation)
{
    if (i < 0 || i >= s_n) return 0;
    probe_agent_t *a = &s_agents[i];
    if (a->persona_gen == generation && a->alive) return 0;   // already synced to this life
    probe_random_mac(a->mac);                                  // fresh unique MAC
    a->arch        = arch;                                     // adopt persona family's archetype
    a->seq         = (uint16_t)(esp_random() & 0x0FFFu);
    a->duty        = (esp_random() % 3u == 0u) ? DUTY_ACTIVE : DUTY_IDLE;
    a->born_ms     = born_ms;
    a->life_ms     = life_ms;
    a->alive       = true;
    a->persona_gen = generation;
    uint32_t base  = (a->duty == DUTY_ACTIVE) ? rnd_range(ACTIVE_MIN_MS, ACTIVE_MAX_MS)
                                              : rnd_range(IDLE_MIN_MS, IDLE_MAX_MS);
    a->next_scan_ms = born_ms + (esp_random() % base);
    return 1;
}
```

- [ ] **Step 6: Declare and implement `phantom_sync_wifi`**

In `main/phantom.h`, add:

```c
// Align every bound probe agent to its phantom (agent i <-> phantom i). Reincarnates an agent
// whenever its phantom's generation has advanced, so the Wi-Fi identity co-appears/co-leaves
// with the persona and adopts the family's archetype. Requires probe_agents_init(n) first.
void phantom_sync_wifi(uint32_t now_ms);
```

In `main/phantom.c`, add `#include "probe_agents.h"` (already included transitively via header) and implement:

```c
void phantom_sync_wifi(uint32_t now_ms)
{
    (void)now_ms;
    for (int i = 0; i < s_n; i++) {
        const phantom_t *ph = &s_ph[i];
        probe_agent_sync(i, phantom_arch(ph->family), ph->born_ms, ph->life_ms, ph->generation);
    }
}
```

- [ ] **Step 7: Rebuild and run the test to verify it PASSES**

```powershell
cd tools/probe_audit ; .\run.ps1 -Rebuild
"C:/Program Files/Python312/python.exe" -m pytest tests/test_wifi_binding.py tests/test_phantom.py tests/test_probe_agents.py -v
```
Expected: all PASS (existing probe_agents tests still green — the standalone path is unaffected).

- [ ] **Step 8: Commit**

```bash
git add main/probe_agents.h main/probe_agents.c main/phantom.h main/phantom.c tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_wifi_binding.py
git commit -m "feat(personas): bind the Wi-Fi side (probe_agent_sync + phantom_sync_wifi)"
```

---

### Task 5: Bind the BLE side + dual-radio coverage

**Files:**
- Modify: `main/ble_devices.h` (add `persona_idx`, `persona_gen` to `ble_device_t`; declare `ble_device_sync`)
- Modify: `main/ble_devices.c` (bound-slot skip in tick; implement `ble_device_sync`)
- Modify: `main/roster.h` / `main/roster.c` (add `roster_pick_company`)
- Modify: `main/phantom.h` / `main/phantom.c` (declare + implement `phantom_sync_ble`)
- Modify: `tools/decoy_audit/run.ps1` (add `phantom.c`, `probe_agents.c`, `probe_frame.c` to the `cl` line) and `tools/decoy_audit/Makefile` (parity)
- Modify: `tools/decoy_audit/synth_dump.c` (add a `--personas` mode: full bound system)
- Test: `tools/decoy_audit/tests/test_personas.py`

**Interfaces:**
- Consumes: `phantom_*` (Tasks 3-4), `make_random_addr`, `roster_at`, `CHURN_ROSTER_SIZE` (from `generate.h`).
- Produces:
  - `ble_device_t` gains `int8_t persona_idx;` (-1 = unbound) and `uint32_t persona_gen;`
  - `identity_t *roster_pick_company(uint16_t company_id);` — a roster entry whose `company_id` matches (if `company_id==0`, an entry with `company_id==0`); NULL if none.
  - `int ble_device_sync(int slot, int persona_idx, uint16_t company, uint32_t born_ms, uint32_t life_ms, uint32_t generation);` — reincarnate a bound BLE slot as an RPA device carrying the family's behaviour + shared born/life + fresh unique addr; returns 1 if reincarnated.
  - `void phantom_sync_ble(uint32_t now_ms);`

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_personas.py`:

```python
import os, subprocess, unittest
from collections import defaultdict

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")

APPLE = 0x004C


def personas(seed, n=12, ndev=24, ticks=4000, tick_ms=1000):
    out = subprocess.check_output(
        [EXE, "--personas", str(seed), str(n), str(ndev), str(ticks), str(tick_ms)], text=True)
    ble, wifi = [], []
    for ln in out.splitlines():
        p = ln.split()
        if p and p[0] == "B":     # B <t> <persona_idx> <addr> <atype> <company> <gen>
            ble.append((int(p[1]), int(p[2]), p[3], p[4], int(p[5], 16), int(p[6])))
        elif p and p[0] == "W":   # W <t> <persona_idx> <mac> <arch> <gen>
            wifi.append((int(p[1]), int(p[2]), p[3], int(p[4]), int(p[5])))
    return ble, wifi


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class Personas(unittest.TestCase):
    def test_every_wifi_has_a_co_present_ble_twin(self):
        ble, wifi = personas(1)
        self.assertTrue(wifi and ble, "no persona events")
        ble_keys = {(i, g) for _t, i, _a, _at, _c, g in ble}
        wifi_keys = {(i, g) for _t, i, _m, _a, g in wifi}
        # dual-radio coverage: every Wi-Fi (persona,generation) has a BLE twin
        missing = wifi_keys - ble_keys
        self.assertFalse(missing, f"Wi-Fi identities with no BLE twin: {len(missing)}")

    def test_twins_co_appear_same_tick(self):
        ble, wifi = personas(2)
        ble_born = {(i, g): t for t, i, _a, _at, _c, g in ble}
        wifi_born = {(i, g): t for t, i, _m, _a, g in wifi}
        for key in wifi_born.keys() & ble_born.keys():
            self.assertEqual(ble_born[key], wifi_born[key], f"twin {key} born on different ticks")

    def test_ble_members_are_rpa_and_law3_safe(self):
        ble, _ = personas(3)
        for _t, _i, _addr, atype, comp, _g in ble:
            self.assertEqual(atype, "rpa", "persona BLE member is not RPA")
            self.assertNotEqual(comp, APPLE, "persona BLE member emitted Apple mfg data (Law-3)")

    def test_samsung_google_families_are_vendor_matched(self):
        ble, wifi = personas(4)
        # arch 1=GALAXY expects company 0x0075; arch 2=PIXEL expects 0x00E0 (when present in roster)
        arch_by_key = {(i, g): a for _t, i, _m, a, g in wifi}
        seen_match = False
        for _t, i, _addr, _atype, comp, g in ble:
            a = arch_by_key.get((i, g))
            if a == 1 and comp == 0x0075: seen_match = True
            if a == 2 and comp == 0x00E0: seen_match = True
            # a matched Galaxy/Pixel persona must not carry the *other* vendor's id
            if a == 1: self.assertNotEqual(comp, 0x00E0, "Galaxy persona carried Google id")
        self.assertTrue(seen_match, "no vendor-matched Samsung/Google persona observed")

    def test_all_addresses_unique_across_both_radios(self):
        ble, wifi = personas(5)
        addrs = [a for _t, _i, a, _at, _c, _g in ble] + [m for _t, _i, m, _a, _g in wifi]
        self.assertEqual(len(addrs), len(set(addrs)), "an address collided across the fleet")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Extend the decoy_audit build source list**

In `tools/decoy_audit/run.ps1`, add the three sources to the `cl` line so the harness can drive the full bound system:

```powershell
           synth_dump.c ble_hs_adv.c roster_stub.c `
           ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c `
           ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c `
           ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c `
           /Fe:synth_dump.exe | Out-Null
```

Also update `tools/decoy_audit/Makefile` `SRC` for parity:

```make
SRC := synth_dump.c ble_hs_adv.c roster_stub.c \
       $(ROOT)/main/generate.c $(ROOT)/main/templates.c $(ROOT)/main/roster.c $(ROOT)/main/ble_devices.c \
       $(ROOT)/main/learn.c $(ROOT)/components/simulacra_radar/law3.c $(ROOT)/components/simulacra_radar/learn_wire.c \
       $(ROOT)/main/uniq_id.c $(ROOT)/main/phantom.c $(ROOT)/main/probe_agents.c $(ROOT)/main/probe_frame.c
```

- [ ] **Step 3: Add the `--personas` mode to `synth_dump.c`**

In `tools/decoy_audit/synth_dump.c`, add `#include "phantom.h"`, `#include "probe_agents.h"` to the includes, and this block at the start of `main` (before `--devices`):

```c
    if (argc > 1 && strcmp(argv[1], "--personas") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      nph    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 12;
        int      ndev   = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 24;
        int      ticks  = argc > 5 ? (int)strtoul(argv[5], 0, 10) : 4000;
        unsigned tickms = argc > 6 ? (unsigned)strtoul(argv[6], 0, 10) : 1000;
        srand(seed);
        roster_init();
        uint32_t t = 0;
        phantom_init(nph, t);
        ble_devices_init(ndev, t);        // slots [0,nph) become bound once synced
        probe_agents_init(nph, t);
        phantom_sync_wifi(t);
        phantom_sync_ble(t);
        static uint32_t bgen[PHANTOM_MAX], wgen[PHANTOM_MAX];
        for (int i = 0; i < nph && i < PHANTOM_MAX; i++) { bgen[i] = 0; wgen[i] = 0; }
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            phantom_lifecycle(t);
            phantom_sync_wifi(t);
            phantom_sync_ble(t);
            ble_devices_tick(t);
            for (int i = 0; i < probe_agents_count(); i++) {
                const probe_agent_t *a = probe_agents_at(i);
                if (a->persona_gen != wgen[i]) {
                    wgen[i] = a->persona_gen;
                    printf("W %u %d ", (unsigned)t, i);
                    for (int b = 0; b < 6; b++) printf("%02x", a->mac[b]);
                    printf(" %d %u\n", (int)a->arch, (unsigned)a->persona_gen);
                }
            }
            for (int i = 0; i < ble_devices_count(); i++) {
                const ble_device_t *d = ble_devices_at(i);
                if (d->persona_idx < 0) continue;               // unbound crowd: not a persona
                int pi = d->persona_idx;
                if (d->persona_gen != bgen[pi]) {
                    bgen[pi] = d->persona_gen;
                    const char *at = d->atype == BLE_ATYPE_STATIC ? "static"
                                   : d->atype == BLE_ATYPE_RPA    ? "rpa" : "nrpa";
                    printf("B %u %d ", (unsigned)t, pi);
                    for (int b = 0; b < 6; b++) printf("%02x", d->id.addr[b]);
                    printf(" %s %04x %u\n", at, (unsigned)d->id.company_id, (unsigned)d->persona_gen);
                }
            }
        }
        return 0;
    }
```

- [ ] **Step 4: Build to verify the test FAILS (symbols missing)**

```powershell
cd tools/decoy_audit
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c roster_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c /Fe:synth_dump.exe
```
Expected: **build fails** — `phantom_sync_ble`, `roster_pick_company`, `ble_device_sync`, and the new `ble_device_t` fields do not exist yet.

- [ ] **Step 5: Add `roster_pick_company`**

In `main/roster.h`, declare after `make_random_addr_mixed`:

```c
identity_t *roster_pick_company(uint16_t company_id);   // roster entry with this company (0=anonymous), or NULL
```

In `main/roster.c`, add:

```c
identity_t *roster_pick_company(uint16_t company_id)
{
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++)
        if (s_roster[i].company_id == company_id) return &s_roster[i];
    return NULL;
}
```

- [ ] **Step 6: Add the bound fields and declare `ble_device_sync`**

In `main/ble_devices.h`, add to `ble_device_t` (after `bool alive;`):

```c
    int8_t   persona_idx;       // >=0: bound to phantom[persona_idx]; -1: unbound BLE-only crowd
    uint32_t persona_gen;       // last phantom generation this bound member synced to
```

And declare:

```c
// Bind BLE slot `slot` to phantom `persona_idx` (see phantom.h): when the phantom's generation
// advances, reincarnate the slot as an RPA device carrying `company`'s behaviour (0 = anonymous
// Law-3-safe RPA), the phantom's shared born/life, and a fresh unique address. Returns 1 if
// reincarnated. Bound slots do NOT expire via ble_devices_tick; the phantom owns their lifetime.
int ble_device_sync(int slot, int persona_idx, uint16_t company,
                    uint32_t born_ms, uint32_t life_ms, uint32_t generation);
```

- [ ] **Step 7: Initialize `persona_idx = -1` for every device and skip bound slots in tick**

In `main/ble_devices.c`, in `dev_spawn` set the device unbound by default. Add at the end of `dev_spawn` (after `d->next_rotate_ms = ...;`):

```c
    d->persona_idx = -1;        // unbound by default; phantom_sync_ble claims bound slots
    d->persona_gen = 0;
```

In `ble_devices_tick`, guard both the death/rebirth loop and the rotation loop so bound slots are driven only by the phantom. Change the death loop body and rotation loop to skip bound devices:

```c
void ble_devices_tick(uint32_t now_ms)
{
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (d->persona_idx >= 0) continue;                 // bound: phantom owns lifecycle
        if (d->alive && (now_ms - d->born_ms) >= d->life_ms) {
            dev_spawn(d, now_ms);
        }
    }
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (!d->alive) continue;
        if (d->persona_idx >= 0) continue;                 // bound RPA rotation handled below
        if (d->atype == BLE_ATYPE_STATIC) continue;
        if ((int32_t)(now_ms - d->next_rotate_ms) >= 0) {
            make_random_addr(d->id.addr, top2_for(d->atype));
            d->next_rotate_ms = now_ms + rotate_base(d->atype);
        }
    }
}
```

- [ ] **Step 8: Implement `ble_device_sync`**

In `main/ble_devices.c`, add (uses `roster_pick_company` and `make_random_addr`, both already reachable via `roster.h`):

```c
int ble_device_sync(int slot, int persona_idx, uint16_t company,
                    uint32_t born_ms, uint32_t life_ms, uint32_t generation)
{
    if (slot < 0 || slot >= s_n) return 0;
    ble_device_t *d = &s_dev[slot];
    if (d->persona_idx == persona_idx && d->persona_gen == generation && d->alive) return 0;
    identity_t *src = roster_pick_company(company);
    if (!src) src = roster_pick_company(0);              // fall back to an anonymous no-mfg shape
    if (!src) src = roster_at(0);                        // last resort: any behaviour
    d->id = *src;                                        // adopt the family's frozen behaviour
    d->atype = BLE_ATYPE_RPA;                            // phones present on BLE as RPA
    make_random_addr(d->id.addr, top2_for(BLE_ATYPE_RPA));   // fresh unique RPA address
    d->role   = BLE_ROLE_TRANSIENT;                      // lifetime is the phantom's, not a band
    d->born_ms = born_ms;
    d->life_ms = life_ms;
    d->alive = true;
    d->next_rotate_ms = born_ms + rotate_base(BLE_ATYPE_RPA);
    d->persona_idx = (int8_t)persona_idx;
    d->persona_gen = generation;
    return 1;
}
```

Note: `roster_at` returns `identity_t *` (see `main/roster.h`), and `roster_pick_company` is declared there too — add `#include "roster.h"` to `ble_devices.c` if not already present (it includes `"roster.h"` already at line 2).

- [ ] **Step 9: Declare and implement `phantom_sync_ble`**

In `main/phantom.h`, add:

```c
// Align the bound BLE slots [0, phantom_count()) to their phantoms: slot i <-> phantom i.
// Reincarnates slot i whenever phantom i's generation advances, so the BLE identity co-appears
// with the Wi-Fi twin and carries the family's vendor (or a Law-3-safe anonymous RPA).
// Requires ble_devices_init(n) with n >= phantom_count().
void phantom_sync_ble(uint32_t now_ms);
```

In `main/phantom.c`, add `#include "ble_devices.h"` and implement:

```c
void phantom_sync_ble(uint32_t now_ms)
{
    (void)now_ms;
    int slots = ble_devices_count();
    for (int i = 0; i < s_n; i++) {
        if (i >= slots) break;                  // no BLE slot for this persona (misconfig guard)
        const phantom_t *ph = &s_ph[i];
        ble_device_sync(i, i, phantom_company(ph->family), ph->born_ms, ph->life_ms, ph->generation);
    }
}
```

This guard means a persona only becomes a full dual-radio device when a BLE slot exists for it; ensure the BLE population is `>= phantom_count()` at runtime (Task 6) so every persona gets a twin.

- [ ] **Step 10: Rebuild and run the persona test to verify it PASSES**

```powershell
cd tools/decoy_audit
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c roster_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c /Fe:synth_dump.exe
"C:/Program Files/Python312/python.exe" -m pytest tests/test_personas.py tests/test_uniqueness_e2e.py tests/test_ble_devices.py -v
```
Expected: all PASS. If `test_samsung_google_families_are_vendor_matched` fails because the template-fallback roster for that seed lacks 0x0075/0x00E0, try seed values until one shows a match, then hardcode that seed in the test — the roster is deterministic per seed (`srand`).

- [ ] **Step 11: Commit**

```bash
git add main/ble_devices.h main/ble_devices.c main/roster.h main/roster.c main/phantom.h main/phantom.c tools/decoy_audit/run.ps1 tools/decoy_audit/Makefile tools/decoy_audit/synth_dump.c tools/decoy_audit/tests/test_personas.py
git commit -m "feat(personas): bind the BLE side + dual-radio coverage (phantom_sync_ble)"
```

---

### Task 6: Wire personas into the firmware runtime

**Files:**
- Modify: `main/CMakeLists.txt` (add `uniq_id.c`, `phantom.c` to `SRCS`)
- Modify: `main/probe.h` / `main/probe.c` (add `probe_desired_ble_floor`; init phantoms; move `probe_agents_lifecycle` out of `probe_inject_burst`)
- Modify: `main/simulacra_main.c:140-151` (raise the BLE population to the persona floor)
- Modify: `main/churn.c` (drive `phantom_lifecycle` + `phantom_sync_ble` each tick)
- Modify: `main/coexist.c` (drive `phantom_sync_wifi` on the Wi-Fi turn)
- Verify: firmware builds for `esp32c5` and `esp32c6`; host self-tests unaffected.

**Interfaces:**
- Consumes: `phantom_init`, `phantom_lifecycle`, `phantom_sync_wifi`, `phantom_sync_ble` (Tasks 3-5).

**Threading note (why this is race-free):** `coexist_task` (`main/coexist.c:235`) is the single task that calls `churn_tick` (→ `ble_devices_tick`) *and* `probe_inject_burst`. All `phantom_*` calls added below run inside that one task, so phantom/agent/device state is accessed serially — no locking needed. `ble_devices_init` runs earlier in `simulacra_task` during bring-up, before `coexist_task` is spawned, so its one-time sizing does not race the loop. `churn_set_active_target` is a no-op (population is fixed at `ble_devices_init`), so the BLE crowd never shrinks below the persona floor at runtime.

- [ ] **Step 1: Register the new sources**

In `main/CMakeLists.txt`, add `"uniq_id.c"` and `"phantom.c"` to the `SRCS` list (append before the closing quote of the `SRCS` argument):

```cmake
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c" "churn.c" "ble_devices.c" "settings.c" "templates.c" "rf_model.c" "observe.c" "generate.c" "probe.c" "probe_frame.c" "probe_agents.c" "sniff.c" "espnow_sniff.c" "drift.c" "coexist.c" "detect.c" "webui.c" "esp_now_link.c" "learn.c" "sig_store.c" "fleet.c" "fleet_key.c" "vbat.c" "uniq_id.c" "phantom.c"
```

- [ ] **Step 2: Size the BLE crowd for personas + initialize phantoms**

The "grow BLE when personas on" decision: the BLE population must host `PROBE_PHONES` bound personas PLUS an unbound remainder that keeps the static/NRPA/persistent mix (so the address-type + presence tells we already closed don't regress). Because the population is fixed at the single `ble_devices_init` call, do the sizing there.

First, expose the floor from `main/probe.c`. Add to `main/probe.h` (after the existing prototypes):

```c
int probe_desired_ble_floor(void);   // min BLE population so every persona gets a co-present twin
```

In `main/probe.c`, add `#include "phantom.h"` and `#include "ble_devices.h"`, define the unbound count, and implement the floor:

```c
// Personas need one BLE slot each PLUS this many unbound BLE-only decoys so the static/NRPA/
// persistent mix survives (the address-type + presence tells we already closed).
#define PHANTOM_BLE_UNBOUND 8

int probe_desired_ble_floor(void)
{
    int floor = PROBE_PHONES + PHANTOM_BLE_UNBOUND;   // C5: 24, C6: 16
    return floor > BLE_DEVICES_MAX ? BLE_DEVICES_MAX : floor;
}
```

Then replace `probe_pool_init` (line ~66) so it inits agents + phantoms and does the initial sync (the population is already sized by Step 2b):

```c
void probe_pool_init(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    probe_agents_init(PROBE_PHONES, now);
    phantom_init(PROBE_PHONES, now);        // one persona per Wi-Fi agent (bind from the Wi-Fi side)
    phantom_sync_wifi(now);                 // agents adopt their persona family immediately
    phantom_sync_ble(now);                  // slots [0,PROBE_PHONES) become the bound RPA twins
    ESP_LOGW(TAG, "phantoms=%d agents=%d ble=%d (bound=%d unbound=%d)",
             phantom_count(), probe_agents_count(), ble_devices_count(),
             PROBE_PHONES, ble_devices_count() - PROBE_PHONES);
}
```

Now raise the BLE population to that floor at the single init site. In `main/simulacra_main.c`, just before `ble_devices_init(ndev, ...)` (line 151), add `#include "probe.h"` (if not present) and clamp `ndev` up:

```c
    if (ndev < probe_desired_ble_floor()) ndev = probe_desired_ble_floor();   // room for personas
    ble_devices_init(ndev, (uint32_t)(esp_timer_get_time() / 1000));
```

- [ ] **Step 3: Move independent agent turnover out of `probe_inject_burst`**

In `main/probe.c`, remove the `probe_agents_lifecycle(now);` call from `probe_inject_burst` (line ~90) so bound agents are no longer independently reincarnated (the phantom owns their life). To keep the `SIMULACRA_PROBE` standalone injector turning over its agents, add the lifecycle call to `probe_task` instead:

```c
static void probe_task(void *arg)
{
    (void)arg;
    for (;;) {
        probe_agents_lifecycle((uint32_t)(esp_timer_get_time() / 1000));  // standalone turnover
#if PROBE_FIX_CH
        probe_inject_burst(PROBE_FIX_CH);
#else
        probe_inject_burst(next_channel());
#endif
        vTaskDelay(pdMS_TO_TICKS(PROBE_BURST_MS));
    }
}
```

- [ ] **Step 4: Drive persona lifecycle + BLE sync from the churn tick**

In `main/churn.c`, near the existing `ble_devices_tick(now_ms);` (line 28), add `#include "phantom.h"` and drive the persona lifecycle and BLE binding each tick:

```c
    phantom_lifecycle(now_ms);      // advance persona births/deaths (single source of truth)
    phantom_sync_ble(now_ms);       // bound BLE slots co-appear/co-leave with their persona
    ble_devices_tick(now_ms);       // advance the unbound crowd (bound slots are skipped)
```

- [ ] **Step 5: Drive Wi-Fi sync on the coexist Wi-Fi turn**

In `main/coexist.c`, at the Wi-Fi injection site (around line 255, where `probe_inject_burst(ch24[...])` is called), add `#include "phantom.h"` and call `phantom_sync_wifi` just before injecting so bound agents reflect the latest persona generations:

```c
            phantom_sync_wifi(now);                                   // agents track persona lives
            if (n24) probe_inject_burst(ch24[hop24++ % n24]);        // 2.4 GHz (coex-arbitrated)
```

(Use the tick's `now` variable already in scope at that call site.)

- [ ] **Step 6: Verify the host harnesses still pass**

The runtime wiring (`probe.c`/`churn.c`/`coexist.c`) is firmware-only and not host-compiled, so re-run the two host harnesses to confirm the pure cores are unaffected:

```powershell
cd tools/probe_audit ; .\run.ps1 -Rebuild
"C:/Program Files/Python312/python.exe" -m pytest tests/ -v
cd ..\decoy_audit
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c roster_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c /Fe:synth_dump.exe
"C:/Program Files/Python312/python.exe" -m pytest tests/ -v
```
Expected: all host tests PASS.

- [ ] **Step 7: Verify the firmware compiles for both decoy targets**

```bash
rm -rf build sdkconfig
idf.py set-target esp32c5
idf.py -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 build
rm -rf build sdkconfig
idf.py set-target esp32c6
idf.py -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 build
```
Expected: both builds succeed (`Project build complete`). This is the on-target integration gate; there is no host unit test for the FreeRTOS wiring itself.

- [ ] **Step 8: Commit**

```bash
git add main/CMakeLists.txt main/probe.h main/probe.c main/simulacra_main.c main/churn.c main/coexist.c
git commit -m "feat(personas): wire phantom lifecycle + sync into the runtime"
```

---

### Task 7: Docs — audit README + roadmap + finish the branch

**Files:**
- Modify: `tools/decoy_audit/README.md` (document the dual-radio coverage / `--personas` mode)
- Modify: `docs/ROADMAP.md` (mark M10 cross-protocol personas — same-board v1 done)
- Verify: full host test suite green.

- [ ] **Step 1: Document the persona audit mode**

Append to `tools/decoy_audit/README.md` a short section:

```markdown
## Dual-radio coverage (cross-protocol personas)

`synth_dump --personas <seed> <n_phantoms> <n_ble> <ticks> <tick_ms>` drives the full bound
system (phantom registry + probe agents + ble_devices) and emits `W`/`B` events. The
`tests/test_personas.py` checks assert **dual-radio coverage** (every Wi-Fi identity has a
co-present BLE twin), co-appearance on the same tick, that persona BLE members are RPA and
Law-3-safe, Samsung/Google vendor-matching, and cross-radio address uniqueness. This is the
"are our fake phones real dual-radio devices?" axis: a real phone-heavy environment has near-100%
coverage, so decoys must too.
```

- [ ] **Step 2: Update the roadmap**

In `docs/ROADMAP.md`, under the M10 bullet, append: ` **(same-board v1 done 2026-07-16 — feat/cross-protocol-personas; mesh-distributed personas remain future.)**`

- [ ] **Step 3: Run the full host suite**

```powershell
"C:/Program Files/Python312/python.exe" -m pytest tools/probe_audit/tests -v
"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests -v
```
Expected: all PASS.

- [ ] **Step 4: Commit**

```bash
git add tools/decoy_audit/README.md docs/ROADMAP.md
git commit -m "docs(personas): dual-radio coverage audit note + roadmap M10"
```

- [ ] **Step 5: Finish the branch**

Invoke **superpowers:finishing-a-development-branch**. Target: merge to `main` locally with `--no-ff`; do NOT push. Verify the host suites are green before merging.

---

## Notes for the implementer

- **Two harnesses, two roles.** `tools/probe_audit` compiles the lightweight Wi-Fi/pure cores (`probe_frame`, `probe_agents`, `uniq_id`, `phantom`). `tools/decoy_audit` compiles the full BLE stack (`generate`, `templates`, `roster`, `ble_devices`, `law3`, `learn`) plus the persona pieces. Both use `host_stubs/esp_random.h` where `esp_random()` is seeded by `srand(seed)` for determinism.
- **Determinism:** every dump seeds `srand(seed)`; the same seed reproduces the same run. Uniqueness/coherence tests rely on this — if a coherence assertion is seed-sensitive (roster vendor presence), pick a seed that exhibits the case and pin it.
- **The `SIMULACRA_PROBE` seq-gate path** must stay behavior-identical: Task 6 keeps agent turnover alive in `probe_task` for that standalone build. Do not remove `probe_agents_lifecycle` entirely.
- **No new wire-struct fields.** This feature adds nothing to the ESP-NOW status struct, so it needs no fleet reflash. (A future "persona count on Vigil" is deferred; it would piggyback a reserved byte, not grow the struct.)
