# Wi-Fi Probe Behavioral Realism Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fixed, lockstep fake-phone pool with a population of independent, short-lived agents so a SignalTrace-class tool cannot link decoys by sequence number, timing, fingerprint, or constellation.

**Architecture:** A pure, host-testable scheduler (`main/probe_agents.c`) owns agent identity, per-agent sequence counters, jittered scan scheduling, and a birth/death lifecycle. `main/probe.c` stays the live-radio glue: it asks the pure core which agents are due, stamps each agent's own sequence number, and transmits with the chip's internal sequence counter disabled. `main/coexist.c` keeps ownership of radio/BLE arbitration and needs no interface change (it already calls `probe_inject_burst`).

**Tech Stack:** C (ESP-IDF 5.5 on ESP32-C5, 5.4 on ESP32-C6), MSVC `cl` host build for the pure core, Python `unittest` harness under `tools/probe_audit`, Kismet pcapng for on-air validation.

## Global Constraints

- **Public repo** — no absolute local paths, OS usernames, real hardware MACs, or real SSIDs in any committed file. Real-capture artifacts live only in the gitignored `private/`, PII-stripped.
- **Targets** — ESP32-C5 (IDF 5.5, 8 agents, 2.4 + 5 GHz) and ESP32-C6 (IDF 5.4, 4 agents, 2.4 GHz). Per-target agent count stays a compile-time constant; the agent core is target-agnostic.
- **Law 3 preserved** — probe requests remain wildcard-SSID only; `probe_frame.c` and its byte-exact fixtures are NOT modified by this milestone.
- **Host-testable core** — new scheduling logic lives in a pure module (RNG via `esp_random()` — real on target, stubbed on host; clock passed in as `now_ms`). No ESP-IDF radio/timer calls in the pure core.
- **Honest ceiling** — this defeats sequence-linking, timing correlation, fingerprint re-linking, and constellation tracking. It does NOT defeat physical co-location / RSSI / AoA. Do not describe it as invisibility in any artifact.
- **Fleet build flags** — hardware build/flash uses `-DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1` (NOT `-DSIMULACRA_PROBE`). Use raw `idf.py` with the IDF export from the build-flash-read skill; that skill's wrapper does not pass fleet flags.
- **Commit trailer** — end every commit message with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`

---

## File Structure

- `main/probe_agents.h` (new) — agent struct + pure scheduler API.
- `main/probe_agents.c` (new) — the pure scheduler: identity, seq counters, lifecycle, due-selection. Host-buildable.
- `main/probe.c` (modify) — replace the `s_phones` pool + `probe_inject_burst` body with agent-driven emission using `en_sys_seq=false` and per-agent seq stamping; `probe_pool_init`/`probe_phone_count` delegate to the core.
- `main/probe.h` (modify) — add `#include "probe_agents.h"`; docs on `probe_inject_burst` semantics change (comment only; signature unchanged).
- `main/CMakeLists.txt` (modify) — add `probe_agents.c` to `SRCS`.
- `tools/probe_audit/probe_dump.c` (modify) — add an `--agents` simulation subcommand driving the pure core.
- `tools/probe_audit/run.ps1` (modify) — add `probe_agents.c` to the `cl` build line.
- `tools/probe_audit/tests/test_probe_agents.py` (new) — host unit tests for the scheduler.
- `tools/probe_audit/analyzers/` (new) — `seqcheck.py`, `timing.py`, `churn.py`, `README.md`: permanent pcapng validators (operate on a user-supplied capture; embed no capture data).
- `main/coexist.c` — NO change expected (calls `probe_inject_burst` already). A build/flash validates the integration.

---

## Task 1: [GATE] Own the sequence number on hardware

Prove `en_sys_seq=false` actually lets our per-frame sequence number reach the air on both chips. Everything else rides on this. Minimal change to the *existing* pool path; if the chip ignores the flag, STOP and reassess scope before building the core.

**Files:**
- Modify: `main/probe.c` (the `probe_phone_t` struct, `probe_pool_init`, `probe_inject_burst`)

**Interfaces:**
- Produces: proof that `esp_wifi_80211_tx(..., false)` honors `frame[22:23]`; the seq-patch technique reused by Task 6.

- [ ] **Step 1: Give each existing phone a seq counter.** In `main/probe.c`, extend the pool struct and seed it:

```c
typedef struct { uint8_t mac[6]; probe_arch_t arch; uint16_t seq; } probe_phone_t;
```
In `probe_pool_init`, after drawing each phone's MAC/arch, add:
```c
        s_phones[i].seq = (uint16_t)(esp_random() & 0x0FFF);   // random 12-bit base per phone
```

- [ ] **Step 2: Stamp the seq and disable the internal counter.** In `probe_inject_burst`, replace the TX line

```c
        rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, true);
```
with:
```c
        uint16_t sc = (uint16_t)(s_phones[i].seq << 4);        // seq -> bits 4..15, frag=0
        f[22] = (uint8_t)(sc & 0xFF);
        f[23] = (uint8_t)((sc >> 8) & 0xFF);
        s_phones[i].seq = (s_phones[i].seq + 1) & 0x0FFF;
        rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, false); // en_sys_seq=false: use OUR seq
```

- [ ] **Step 3: Build + flash both boards.** Using raw `idf.py` with the fleet flags (IDF export per build-flash-read skill), build/flash the C5 and the C6. Expected: both flash `Hash of data verified`, and serial shows `answered request` + `burst ch=...` (fleet still healthy).

- [ ] **Step 4: Capture + verify seq independence on-air.** Run a short Kismet capture near the fleet (2.4 GHz is enough), save the pcapng to `private/`, then run the existing scratch checker:

Run: `python <scratch>/seqcheck.py <private/NEW.pcapng> tools/probe_audit/fixtures`
Expected: per-MAC sequence lists are **each independently monotonic** and **no consecutive run spans different MACs** (contrast: the pre-fix capture showed 7 MACs at 3793–3799). 

- [ ] **Step 5: Decision gate.**
  - **PASS** → commit and continue.
  - **FAIL** (on-air seq still one shared counter) → STOP. Do not build the core. Record the finding in `private/PROBE-ANTIFINGERPRINT-STANCE.md` and re-scope (the milestone still helps tells #2–#4, but #1 is unachievable in firmware).

- [ ] **Step 6: Commit.**
```bash
git add main/probe.c
git commit -m "feat(probe): own per-phone 802.11 sequence number (en_sys_seq=false)"
```

---

## Task 2: Pure agent core — module + sequence counter (TDD)

**Files:**
- Create: `main/probe_agents.h`, `main/probe_agents.c`
- Modify: `tools/probe_audit/probe_dump.c`, `tools/probe_audit/run.ps1`
- Test: `tools/probe_audit/tests/test_probe_agents.py`

**Interfaces:**
- Produces: `probe_agent_t`, `probe_agents_init`, `probe_agent_next_seq`, `probe_agents_count`, `probe_agents_at` (full API below; lifecycle/due land in Tasks 3–4).

- [ ] **Step 1: Write the header.** Create `main/probe_agents.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "probe_frame.h"

#define PROBE_AGENTS_MAX 12

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
```

- [ ] **Step 2: Write the implementation (seq + init only for now).** Create `main/probe_agents.c`:

```c
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
    a->seq     = (uint16_t)(esp_random() & 0x0FFFu);              // random 12-bit base
    a->duty    = (esp_random() % 4u == 0u) ? DUTY_ACTIVE : DUTY_IDLE;  // ~25% active
    a->born_ms = now_ms;
    a->life_ms = rnd_range(LIFE_MIN_MS, LIFE_MAX_MS);
    a->alive   = true;
    uint32_t base = (a->duty == DUTY_ACTIVE) ? rnd_range(ACTIVE_MIN_MS, ACTIVE_MAX_MS)
                                             : rnd_range(IDLE_MIN_MS, IDLE_MAX_MS);
    a->next_scan_ms = now_ms + (esp_random() % base);             // random phase-in (not all due at once)
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

/* lifecycle + due land in Tasks 3-4 */
int probe_agents_lifecycle(uint32_t now_ms) { (void)now_ms; return 0; }
int probe_agents_due(uint32_t now_ms, probe_agent_t **out, int max) { (void)now_ms; (void)out; (void)max; return 0; }
```

- [ ] **Step 3: Add the `--agents` dumper subcommand.** In `tools/probe_audit/probe_dump.c`, add `#include "probe_agents.h"` and, at the top of `main` (before the `--pick` block):

```c
    if (argc > 1 && strcmp(argv[1], "--agents") == 0) {
        unsigned seed  = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      nag   = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 8;
        int      ticks = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 2000;
        unsigned tickms= argc > 5 ? (unsigned)strtoul(argv[5], 0, 10) : 2000;
        srand(seed);
        uint32_t t = 0;
        probe_agents_init(nag, t);
        for (int s = 0; s < ticks; s++) {
            t += tickms;
            probe_agents_lifecycle(t);
            probe_agent_t *due[PROBE_AGENTS_MAX];
            int nd = probe_agents_due(t, due, PROBE_AGENTS_MAX);
            for (int i = 0; i < nd; i++) {
                uint16_t sq = probe_agent_next_seq(due[i]);
                printf("E %u ", (unsigned)t);
                for (int b = 0; b < 6; b++) printf("%02x", due[i]->mac[b]);
                printf(" %u\n", (unsigned)sq);
            }
        }
        return 0;
    }
```

- [ ] **Step 4: Add the module to the host build.** In `tools/probe_audit/run.ps1`, change the `cl` source list from
`probe_dump.c ..\..\main\probe_frame.c` to
`probe_dump.c ..\..\main\probe_frame.c ..\..\main\probe_agents.c`.

- [ ] **Step 5: Write the failing test.** Create `tools/probe_audit/tests/test_probe_agents.py`:

```python
import os, subprocess, unittest
from collections import defaultdict, Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")

def sim(seed, n=8, ticks=2000, tick_ms=2000):
    out = subprocess.check_output([EXE, "--agents", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 4 and p[0] == "E":
            rows.append((int(p[1]), p[2], int(p[3])))   # t_ms, mac, seq
    return rows

@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Agents(unittest.TestCase):
    def test_seq_monotonic_per_mac(self):
        rows = sim(2)
        seqs = defaultdict(list)
        for _, mac, sq in rows: seqs[mac].append(sq)
        self.assertTrue(seqs, "no emissions produced")
        for mac, ss in seqs.items():
            for a, b in zip(ss, ss[1:]):
                self.assertEqual(b, (a + 1) & 0x0FFF, f"{mac} seq not monotonic {a}->{b}")

    def test_seq_independent_across_macs(self):
        rows = sim(3)
        firsts = {}
        for _, mac, sq in rows: firsts.setdefault(mac, sq)
        self.assertGreater(len(set(firsts.values())), 1, "all agents share a seq base (not independent)")
```

- [ ] **Step 6: Run the test — expect FAIL (no emissions yet, due() is a stub).**

Run: `pwsh tools/probe_audit/run.ps1 -Rebuild`
Expected: `test_seq_monotonic_per_mac` FAILS on "no emissions produced" (the due() stub returns 0). This confirms the harness wiring and the not-yet-implemented scheduler.

- [ ] **Step 7: Commit.**
```bash
git add main/probe_agents.h main/probe_agents.c tools/probe_audit/probe_dump.c tools/probe_audit/run.ps1 tools/probe_audit/tests/test_probe_agents.py
git commit -m "feat(probe): pure agent core scaffold + per-agent sequence counter"
```

---

## Task 3: Pure agent core — lifecycle turnover (TDD)

**Files:**
- Modify: `main/probe_agents.c` (implement `probe_agents_lifecycle`)
- Test: `tools/probe_audit/tests/test_probe_agents.py`

**Interfaces:**
- Consumes: `agent_spawn`, `s_agents`, `s_n` (Task 2).
- Produces: population turnover — expired agents reincarnate with a brand-new MAC/arch/seq/lifetime.

- [ ] **Step 1: Write the failing tests.** Append to `test_probe_agents.py`:

```python
    def test_constellation_turns_over(self):
        rows = sim(4, n=8, ticks=2000, tick_ms=2000)   # ~66 min simulated
        macs = set(m for _, m, _ in rows)
        self.assertGreater(len(macs), 8 * 2, "population did not turn over (stable constellation)")

    def test_mac_lifetime_bounded(self):
        rows = sim(6)
        span = defaultdict(lambda: [10**12, -1])
        for t, mac, _ in rows:
            s = span[mac]; s[0] = min(s[0], t); s[1] = max(s[1], t)
        for mac, (lo, hi) in span.items():
            self.assertLessEqual(hi - lo, 600000 + 180000, f"{mac} outlived its lifetime (stable constellation)")
```

- [ ] **Step 2: Run — expect FAIL** (lifecycle is a stub, so no MAC is ever retired; with `next_scan` never satisfied by the still-stubbed due(), there may be no rows — either way the turnover assertion cannot pass yet).

Run: `pwsh tools/probe_audit/run.ps1`
Expected: `test_constellation_turns_over` FAILS.

- [ ] **Step 3: Implement lifecycle.** Replace the stub in `main/probe_agents.c`:

```c
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
```

- [ ] **Step 4: Run — still expect the turnover test to FAIL** until due-selection exists (no emissions ⇒ no rows to observe turnover). This is expected; lifecycle is correct but unobservable until Task 4.

Run: `pwsh tools/probe_audit/run.ps1`
Expected: turnover/lifetime tests still fail for lack of emissions. (If you prefer strict red-green, defer running these two until Task 4 Step 4; they are grouped here because they exercise `lifecycle`.)

- [ ] **Step 5: Commit.**
```bash
git add main/probe_agents.c tools/probe_audit/tests/test_probe_agents.py
git commit -m "feat(probe): agent lifecycle — bounded lifetimes + reincarnation turnover"
```

---

## Task 4: Pure agent core — due selection + duty cadence (TDD)

**Files:**
- Modify: `main/probe_agents.c` (implement `probe_agents_due`)
- Test: `tools/probe_audit/tests/test_probe_agents.py`

**Interfaces:**
- Consumes: `s_agents`, `s_n`, duty constants (Task 2).
- Produces: a partial, reschedule-on-select due subset — the decorrelation guarantee.

- [ ] **Step 1: Write the failing tests.** Append to `test_probe_agents.py`:

```python
    def test_never_all_fire_same_tick(self):
        rows = sim(1, n=8)
        per_tick = Counter(t for t, _, _ in rows)
        self.assertTrue(per_tick, "no emissions produced")
        self.assertLess(max(per_tick.values()), 8, "a tick fired ALL agents at once (lockstep)")

    def test_active_idle_cadence_separation(self):
        rows = sim(7)
        cnt = Counter(m for _, m, _ in rows)
        counts = sorted(cnt.values(), reverse=True)
        self.assertGreater(len(counts), 1, "too few distinct agents to compare cadence")
        self.assertGreater(counts[0], counts[-1], "no active/idle cadence separation")
```

- [ ] **Step 2: Run — expect FAIL** (due() still returns 0 → "no emissions produced").

Run: `pwsh tools/probe_audit/run.ps1`
Expected: `test_never_all_fire_same_tick` FAILS.

- [ ] **Step 3: Implement due-selection.** Replace the stub in `main/probe_agents.c`:

```c
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
```

- [ ] **Step 4: Run the full suite — expect PASS.**

Run: `pwsh tools/probe_audit/run.ps1 -Rebuild`
Expected: all `test_probe_agents.py` tests PASS (seq monotonic + independent, turnover, bounded lifetime, never-all-at-once, cadence separation), and the existing `test_probe_frame.py` still PASS.
Note: if `test_active_idle_cadence_separation` is too tight for a given seed, widen it or pick another fixed seed — the mechanism, not the exact ratio, is what's under test.

- [ ] **Step 5: Commit.**
```bash
git add main/probe_agents.c tools/probe_audit/tests/test_probe_agents.py
git commit -m "feat(probe): due-selection + duty cadence (decorrelated partial subset)"
```

---

## Task 5: Graduate pcapng analyzers into the harness

Turn the scratch scripts into permanent, PII-safe on-air validators used by Task 6.

**Files:**
- Create: `tools/probe_audit/analyzers/seqcheck.py`, `timing.py`, `churn.py`, `README.md`

**Interfaces:**
- Consumes: a user-supplied Kismet `.pcapng` path (argv) + `tools/probe_audit/fixtures`.
- Produces: three pass/fail reports for the Task 6 gate.

- [ ] **Step 1: Add `seqcheck.py`.** Port the scratch `seqcheck.py` (walk EPB → strip radiotap by len → filter FC `40 00` → SA=`frame[10:16]`, seq=`(u16le(frame[22:24])>>4)&0xFFF`, body=`frame[24:]`; label decoys by matching `norm_ds(body)[:-4]` — FCS-stripped — against the fixtures). Print PASS/FAIL: **FAIL if any consecutive seq run of length ≥ 3 spans ≥ 2 distinct decoy MACs.**

- [ ] **Step 2: Add `timing.py`.** Per decoy MAC, compute inter-emission intervals; over all decoys, report the count of "volley" instants (≥ 4 distinct decoy MACs within a 10 ms window). Print PASS/FAIL: **FAIL if any volley of ≥ 4 MACs occurs** (real crowds don't fire in lockstep).

- [ ] **Step 3: Add `churn.py`.** Bucket the capture into thirds by timestamp; report the decoy-MAC set per third and the Jaccard overlap between first and last third. Print PASS/FAIL: **FAIL if first⇔last overlap > 0.5** (the constellation didn't turn over). Note in output that a short capture may under-report churn.

- [ ] **Step 4: Add `README.md`.** Document usage (`python analyzers/seqcheck.py <capture.pcapng>`), the three success criteria verbatim from the spec §7, and a bold reminder: captures stay in `private/`; never commit one.

- [ ] **Step 5: Smoke-run against the Task 1 capture.** Point `seqcheck.py` at the `private/` pcapng from Task 1.

Run: `python tools/probe_audit/analyzers/seqcheck.py <private/NEW.pcapng>`
Expected: it runs and prints a PASS/FAIL verdict without error (correctness of the verdict is confirmed in Task 6 against the full agent build).

- [ ] **Step 6: Commit.**
```bash
git add tools/probe_audit/analyzers
git commit -m "test(probe): permanent pcapng analyzers — seq/timing/constellation"
```

---

## Task 6: Live glue — agent-driven injection + on-air validation

Replace the pool with the agent core in the live path, then prove the three tells are gone on-air. `coexist.c` is unchanged (it already calls `probe_inject_burst`).

**Files:**
- Modify: `main/probe.c` (`probe_pool_init`, `probe_inject_burst`, `probe_phone_count`, remove `s_phones`/`PROBE_ROTATE_EVERY`)
- Modify: `main/probe.h` (add include + comment)
- Modify: `main/CMakeLists.txt` (add `probe_agents.c`)

**Interfaces:**
- Consumes: `probe_agents_init`, `probe_agents_lifecycle`, `probe_agents_due`, `probe_agent_next_seq`, `probe_agents_count` (Tasks 2–4).

- [ ] **Step 1: Add the source to CMake.** In `main/CMakeLists.txt`, add `"probe_agents.c"` to `SRCS` (next to `"probe_frame.c"`).

- [ ] **Step 2: Include the core.** In `main/probe.h`, add under the existing include: `#include "probe_agents.h"`. Update the `probe_inject_burst` comment to: "Inject one agent tick on `channel`: advance lifecycle, TX a probe-req from each *due* fake phone with its own sequence number, `en_sys_seq=false`."

- [ ] **Step 3: Rewrite the pool + burst in `main/probe.c`.** Remove the `probe_phone_t s_phones[...]`, `s_n`, and `PROBE_ROTATE_EVERY`. Replace `probe_pool_init`, `probe_inject_burst`, and `probe_phone_count` with:

```c
void probe_pool_init(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    probe_agents_init(PROBE_PHONES, now);
    ESP_LOGW(TAG, "probe agents: %d (independent seq, jittered scan, lifecycle churn)",
             probe_agents_count());
}

int probe_phone_count(void) { return probe_agents_count(); }

int probe_inject_burst(uint8_t channel)
{
    bool band5 = (channel >= 36);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    probe_agents_lifecycle(now);
    int crc = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    probe_agent_t *due[PROBE_MAX_PHONES];
    int nd = probe_agents_due(now, due, PROBE_MAX_PHONES);
    int rc = 0, sent = 0;
    for (int i = 0; i < nd; i++) {
        uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
        if (probe_build_request(due[i]->mac, channel, due[i]->arch, band5, f, &n) != 0)
            continue;                                          // archetype lacks this band (defensive)
        uint16_t sc = (uint16_t)(probe_agent_next_seq(due[i]) << 4);   // seq bits 4..15, frag=0
        f[22] = (uint8_t)(sc & 0xFF);
        f[23] = (uint8_t)((sc >> 8) & 0xFF);
        rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, false);         // our per-agent seq
        s_probes_sent++; sent++;
    }
    ESP_LOGW(TAG, "burst ch=%u due=%d/%d band5=%d set_ch_rc=%d tx_rc=%d",
             channel, sent, probe_agents_count(), band5, crc, rc);
    return rc;
}
```
Keep `PROBE_MAX_PHONES` (≥ `PROBE_AGENTS_MAX` bound for the `due[]` array). Leave the dev-mode `next_channel`/`probe_task`/`probe_start` as-is (they call the rewritten functions).

- [ ] **Step 4: Confirm nothing else depends on the old pool.** 

Run: `grep -rn "s_phones\|PROBE_ROTATE_EVERY" main/`
Expected: no matches. Also verify `main/churn_selftest.c` only uses `probe_build_request` (signature unchanged) — no edit needed.

- [ ] **Step 5: Host regression.** 

Run: `pwsh tools/probe_audit/run.ps1 -Rebuild`
Expected: all agent + frame tests still PASS (pure core untouched by the glue rewrite).

- [ ] **Step 6: Build + flash both boards** (fleet flags, raw `idf.py`). Expected: both `Hash of data verified`; serial shows `probe agents: N ...` and `burst ch=... due=k/N ...` with `k < N` on most bursts (partial subsets), plus `fleet: enrolled` and `answered request` (fleet intact).

- [ ] **Step 7: Capture + run the three analyzers.** Fresh Kismet capture near the fleet (2.4 GHz minimum; 5 GHz if an mt7612u is available), pcapng saved to `private/`. Then:

Run:
```
python tools/probe_audit/analyzers/seqcheck.py <private/CAP.pcapng>
python tools/probe_audit/analyzers/timing.py   <private/CAP.pcapng>
python tools/probe_audit/analyzers/churn.py    <private/CAP.pcapng>
```
Expected: **seqcheck PASS** (no cross-MAC seq runs), **timing PASS** (no ≥4-MAC volley), **churn PASS** if the capture is long enough (note if short). Record results in `private/PROBE-ANTIFINGERPRINT-STANCE.md`.

- [ ] **Step 8: Commit.**
```bash
git add main/probe.c main/probe.h main/CMakeLists.txt
git commit -m "feat(probe): agent-driven injection replaces fixed lockstep pool"
```

---

## Task 7: Finish the branch

- [ ] **Step 1:** Confirm host suite green (`pwsh tools/probe_audit/run.ps1 -Rebuild`) and both boards flashed clean.
- [ ] **Step 2:** Update `private/PROBE-ANTIFINGERPRINT-STANCE.md` and the memory `probe-antifingerprint-stance.md` with the on-air results (seq/timing/churn verdicts).
- [ ] **Step 3:** Use superpowers:finishing-a-development-branch to verify tests, present merge/PR options, and complete the branch.

---

## Self-Review

- **Spec coverage:** §1 tells → Tasks 1 (seq), 4 (timing decorrelation), 3 (constellation), agent binding (2). §4 architecture (pure core + glue + coexist untouched) → Tasks 2/6. §5 agent model → Tasks 2–4. §6 seq realism + HW gate → Tasks 1, 2. §7 validation bar → Tasks 5–6. §8 testing → Tasks 2–4. §9 non-goals honored (no BLE, no capture-driven params, best-effort sweep). §3/§10 honest ceiling + constraints → Global Constraints. Covered.
- **Placeholder scan:** none — all steps carry concrete code, commands, and expected output.
- **Type consistency:** `probe_agent_t`, `probe_agents_due(now, out, max)`, `probe_agent_next_seq`, `probe_agents_lifecycle`, `probe_agents_count` used identically in header (Task 2), dumper (Task 2), and glue (Task 6). Seq stamped at `frame[22:23]` consistently in Tasks 1 and 6. `PROBE_PHONES`/`PROBE_MAX_PHONES` retained from existing `probe.c`.
- **Ordering risk:** Task 1 is the hard gate before core work. Tasks 3’s turnover assertions only become observable once Task 4’s due-selection emits — flagged in Task 3 Step 4 so the executor isn’t surprised by red between tasks.
