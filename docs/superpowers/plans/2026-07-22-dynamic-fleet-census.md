# Dynamic Fleet-Size Census Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the static `-DSIMULACRA_FLEET_SIZE=K` divisor at the Wi-Fi population-match reprofile-tick call site with a live census of currently-heard fleet peers (via each peer's real ESP-NOW hardware MAC), so the fleet self-configures without a re-flash.

**Architecture:** A new small peer-node table in `main/fleet.c` (distinct from the existing synthetic-MAC exclusion table), fed by the sender identity (`info->src_addr`) already available on every `RADAR_TYPE_FLEET_MACS` broadcast; a thin `fleet_pop_live_size()` wrapper in `main/fleet_pop.c`; wired into exactly one call site in `main/coexist.c`. The BLE/persona boot-time population sizing is untouched (documented v1 boundary — see spec).

**Tech Stack:** C (ESP-IDF firmware; MSVC `cl` for host tests). Host tests extend `tools/decoy_audit`'s existing `synth_dump` harness (already links `fleet_pop.c` and has `--fleet-share`/`--fleet-size` modes) — `churn_selftest.c` is **on-device only** (not host-compiled anywhere) and is not used here.

## Global Constraints

- `FLEET_NODE_CAP = 8` (peer-node table size); reuse the existing `FLEET_MAC_TTL_MS = 90000` (90 s) for node freshness — no new TTL constant.
- **Scope boundary (from spec):** only `main/coexist.c`'s Wi-Fi reprofile-tick target changes. `main/simulacra_main.c`'s boot-time `phantom_init(fleet_pop_share(probe_phone_target()), ...)` call is **not touched** — it runs before ESP-NOW starts, so no live census is available at that point.
- Self-loopback is a non-issue (ESP-NOW does not deliver a station's own transmission to its own RX callback) — no self-MAC filtering needed.
- `fleet_pop_live_size(now)` = `fleet_node_count(now) + 1`, degrading to `1` (standalone/identity) with no peers heard — this must hold with zero special-casing in the caller.
- Host tests via `"C:/Program Files/Python312/python.exe" -m pytest -q` from `tools/decoy_audit/`; `cl` rebuilds of `synth_dump.exe` from a "Developer PowerShell for VS". Firmware compile-verify **esp32c5** (`idf.py -B build_c5 build` — matches the current root `sdkconfig`, no `set-target`).
- **Never hardcode the OS username / forbidden committer name into a tracked file** (`private/TOOLING-GOTCHAS.md`).
- Commit identity `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>`; every commit ends with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.
- Branch `feat/dynamic-fleet-census`; merge to `main` `--no-ff`, PII-scan, push at the end.
- **HW validation is explicitly out of scope for this plan** (deferred to the user, per the standing no-hardware-changes-while-unattended rule this session) — compile-verify + host tests are what gates the merge.

---

## File Structure

- **Modify** `main/fleet.h` / `main/fleet.c` — the peer-node census table.
- **Modify** `main/fleet_pop.h` / `main/fleet_pop.c` — `fleet_pop_live_size()`.
- **Modify** `main/esp_now_link.c` — note the sender on every `FLEET_MACS` receipt.
- **Modify** `main/coexist.c` — use the live size at the Wi-Fi reprofile-tick call site.
- **Modify** `tools/decoy_audit/synth_dump.c` — new `--fleetnode` harness mode.
- **Modify** `tools/decoy_audit/{Makefile,run.ps1,README.md}` — add `fleet.c` to the build (not currently linked; `fleet_pop.c` already is).
- **Create** `tools/decoy_audit/tests/test_fleet_census.py`.

---

### Task 1: Peer-node census + live fleet size (host-testable)

**Files:**
- Modify: `main/fleet.h`, `main/fleet.c`
- Modify: `main/fleet_pop.h`, `main/fleet_pop.c`
- Modify: `tools/decoy_audit/synth_dump.c`, `tools/decoy_audit/Makefile`, `tools/decoy_audit/run.ps1`, `tools/decoy_audit/README.md`
- Test: `tools/decoy_audit/tests/test_fleet_census.py`

**Interfaces:**
- Produces: `void fleet_note_peer_node(const uint8_t mac[6], uint32_t now_ms)`, `size_t fleet_node_count(uint32_t now_ms)` (`fleet.h`); `int fleet_pop_live_size(uint32_t now_ms)` (`fleet_pop.h`, consumes `fleet_node_count`). Task 2 consumes all three.

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_fleet_census.py`:

```python
import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def fleetnode(script):
    out = subprocess.run([EXE, "--fleetnode"], input=script, capture_output=True, text=True).stdout
    return [int(x) for x in out.split()]


def _macs(n, base=1):
    return "".join("note 02000000%04x 0\n" % (base + i) for i in range(n))


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class FleetCensus(unittest.TestCase):
    def test_no_peers_count_zero_livesize_one(self):
        self.assertEqual(fleetnode("reset\ncount 0\nlivesize 0\n"), [0, 1])

    def test_distinct_nodes_counted(self):
        self.assertEqual(fleetnode("reset\n" + _macs(3) + "count 0\nlivesize 0\n"), [3, 4])

    def test_renote_same_node_no_double_count(self):
        script = "reset\nnote 020000000001 0\nnote 020000000001 100\ncount 100\nlivesize 100\n"
        self.assertEqual(fleetnode(script), [1, 2])

    def test_ttl_expiry_drops_node(self):
        # FLEET_MAC_TTL_MS = 90000: a node last seen at t=0 is gone by t=200000
        script = "reset\nnote 020000000001 0\ncount 200000\nlivesize 200000\n"
        self.assertEqual(fleetnode(script), [0, 1])

    def test_eviction_caps_at_node_cap(self):
        # FLEET_NODE_CAP = 8: noting 12 distinct nodes still reports at most 8
        rows = fleetnode("reset\n" + _macs(12) + "count 0\n")
        self.assertLessEqual(rows[-1], 8)

    def test_independent_of_mac_exclusion_table(self):
        # noting many distinct SYNTHETIC macs via the existing exclusion API must not
        # inflate the NODE count -- they are unrelated tables.
        pass  # covered structurally: --fleetnode never touches fleet_note_peer_macs


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_fleet_census.py -q`
Expected: FAIL — no `--fleetnode` mode exists, `fleetnode()` returns non-conforming output.

- [ ] **Step 3: Add the peer-node table to `main/fleet.h`**

Add after the existing `FLEET_MAC_TTL_MS` block:

```c
#ifndef FLEET_NODE_CAP
#define FLEET_NODE_CAP 8          // distinct peer nodes tracked (real ESP-NOW hardware MACs, not synthetic)
#endif
```

Add after the existing `fleet_peer_count` declaration:

```c
// Note a live peer NODE (its real ESP-NOW hardware sender MAC -- info->src_addr on receipt of a
// FLEET_MACS broadcast), refreshing if already known. Separate table from the synthetic-MAC
// exclusion table above (different purpose: node IDENTITY, not MAC content).
void   fleet_note_peer_node(const uint8_t mac[6], uint32_t now_ms);
// Distinct non-expired peer NODES heard from recently (does not count this node -- ESP-NOW never
// delivers a station's own transmission to its own receive callback).
size_t fleet_node_count(uint32_t now_ms);
```

- [ ] **Step 4: Implement the peer-node table in `main/fleet.c`**

Add after the existing peer-MAC table's statics and functions (after `fleet_macs_unpack`, at the end
of the file), mirroring the exact add/refresh/evict-oldest shape already used for peer MACs:

```c
static struct { uint8_t mac[6]; uint32_t last_ms; bool used; } s_nodes[FLEET_NODE_CAP];

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
```

Also update `fleet_reset(void)` to clear the new table (it currently only does
`memset(s_tbl, 0, sizeof s_tbl)`):

```c
void fleet_reset(void) { memset(s_tbl, 0, sizeof s_tbl); memset(s_nodes, 0, sizeof s_nodes); }
```

- [ ] **Step 5: Add `fleet_pop_live_size` to `main/fleet_pop.h`**

Add after the existing `fleet_pop_share` declaration:

```c
// Live fleet size: distinct peer NODES heard from recently (fleet_node_count), + this node.
// Falls back to 1 (standalone) with no peers heard -- the correct, safe default, achieved for
// free (fleet_node_count returns 0 when nothing has been noted yet).
int fleet_pop_live_size(uint32_t now_ms);
```

- [ ] **Step 6: Implement in `main/fleet_pop.c`**

Add the include and the function:

```c
#include "fleet.h"
```

```c
int fleet_pop_live_size(uint32_t now_ms)
{
    return (int)fleet_node_count(now_ms) + 1;
}
```

- [ ] **Step 7: Add `fleet.c` to the `decoy_audit` build**

`tools/decoy_audit/Makefile` — append `$(ROOT)/main/fleet.c` to the `SRC :=` list (alongside the
existing `$(ROOT)/main/fleet_pop.c` line):

```make
       $(ROOT)/main/fleet_pop.c $(ROOT)/main/fleet.c
```

`tools/decoy_audit/run.ps1` — append `..\..\main\fleet.c` to the `cl` source list right after
`..\..\main\fleet_pop.c`:

```powershell
           ..\..\main\fleet_pop.c ..\..\main\fleet.c `
```

`tools/decoy_audit/README.md` — same addition to the documented `cl` line, after `fleet_pop.c ^`:

```
   ..\..\main\fleet_pop.c ..\..\main\fleet.c ^
```

- [ ] **Step 8: Add the `--fleetnode` harness mode to `tools/decoy_audit/synth_dump.c`**

Add the include with the existing `#include "fleet_pop.h"` (line 7):

```c
#include "fleet.h"
```

Insert this block right after the existing `--fleet-size` block (so it sits with the other
`fleet_pop`-related modes):

```c
    if (argc > 1 && strcmp(argv[1], "--fleetnode") == 0) {
        char line[64], cmd[16], mh[16];
        unsigned u;
        while (fgets(line, sizeof line, stdin)) {
            if (sscanf(line, "%15s", cmd) != 1) continue;
            if (strcmp(cmd, "reset") == 0) {
                fleet_reset();
            } else if (strcmp(cmd, "note") == 0 && sscanf(line, "%*s %12s %u", mh, &u) == 2) {
                uint8_t m[6];
                for (int i = 0; i < 6; i++) { char b[3] = { mh[2 * i], mh[2 * i + 1], 0 }; m[i] = (uint8_t)strtoul(b, 0, 16); }
                fleet_note_peer_node(m, u);
            } else if (strcmp(cmd, "count") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                printf("%d\n", (int)fleet_node_count(u));
            } else if (strcmp(cmd, "livesize") == 0 && sscanf(line, "%*s %u", &u) == 1) {
                printf("%d\n", fleet_pop_live_size(u));
            }
        }
        return 0;
    }
```

- [ ] **Step 9: Rebuild `synth_dump.exe`**

From a Developer PowerShell for VS, in `tools/decoy_audit`:

```powershell
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c roster_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c ..\..\main\fleet_pop.c ..\..\main\fleet.c /Fe:synth_dump.exe
```

Expected: compiles clean.

- [ ] **Step 10: Run the test to verify it passes**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_fleet_census.py -q`
Expected: PASS (6 tests). Then the full suite: `"C:/Program Files/Python312/python.exe" -m pytest -q`
→ all pass (was 97; now 103).

- [ ] **Step 11: Commit**

```bash
git add main/fleet.h main/fleet.c main/fleet_pop.h main/fleet_pop.c \
        tools/decoy_audit/synth_dump.c tools/decoy_audit/Makefile tools/decoy_audit/run.ps1 \
        tools/decoy_audit/README.md tools/decoy_audit/tests/test_fleet_census.py
git commit -F - <<'EOF'
feat(fleet-census): peer-node table + live fleet size (host-tested)

fleet_note_peer_node/fleet_node_count track distinct ESP-NOW sender MACs
(real hardware identity, not synthetic content) in a table separate from
the existing MAC-exclusion table, reusing FLEET_MAC_TTL_MS. fleet_pop_live_size
= fleet_node_count+1, degrading to 1 (standalone) with no peers heard.
Host-tested via synth_dump --fleetnode; fleet.c newly linked into decoy_audit
(fleet_pop.c already was).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 2: Wire the live census into firmware

**Files:**
- Modify: `main/esp_now_link.c` (the `RADAR_TYPE_FLEET_MACS` handler, ~lines 177-186)
- Modify: `main/coexist.c` (the Wi-Fi reprofile-tick target line)

**Interfaces:**
- Consumes: `fleet_note_peer_node`, `fleet_pop_live_size` (Task 1). `fleet_pop_share_k` (pre-existing,
  unchanged signature).

**Verification note:** this task is firmware glue (`esp_now_link.c`/`coexist.c` are not host-compiled)
— verified by compile-verify, not a new automated test. Task 1's host tests already gate the pure
arithmetic/table logic this task wires together.

- [ ] **Step 1: Note the sender in `main/esp_now_link.c`**

In the existing `RADAR_TYPE_FLEET_MACS` handler (~line 177-186), add one line inside the `if (nm)`
block, alongside the existing `fleet_note_peer_macs` call:

```c
    if (type == RADAR_TYPE_FLEET_MACS) {                   // a peer decoy's active synthetic MACs
        uint8_t macs[FLEET_MAC_CAP][6];
        size_t nm = fleet_macs_unpack(pl, plen, macs, FLEET_MAC_CAP);
        if (nm) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            fleet_note_peer_macs(macs, nm, now);
            fleet_note_peer_node(info->src_addr, now);   // real hardware sender identity -> live census
            ESP_LOGW(ETAG, "fleet: peer +%u macs (peers=%u)", (unsigned)nm, (unsigned)fleet_peer_count(now));
        }
        return;
    }
```

(`info` is already the function parameter of `on_recv(const esp_now_recv_info_t *info, ...)` — no
new parameter threading needed.)

- [ ] **Step 2: Use the live size in `main/coexist.c`**

Locate the Wi-Fi reprofile-tick target line (added by the fleet-wifi-popmatch work):

```c
            int wt = s_wifi_obs_ok ? wifi_obs_target(now) : WIFI_OBS_FALLBACK;
            probe_agents_set_target(fleet_pop_share(wt), now);      // /K: fleet emits ~density total, not K*density
```

Change the second line to use the live census instead of the static `-DSIMULACRA_FLEET_SIZE`:

```c
            int wt = s_wifi_obs_ok ? wifi_obs_target(now) : WIFI_OBS_FALLBACK;
            probe_agents_set_target(fleet_pop_share_k(wt, fleet_pop_live_size(now)), now);  // live /K
```

`fleet_pop.h` is already included in `coexist.c` (from the fleet-wifi-popmatch work) — no new
include needed; `fleet_pop_share_k` is declared there alongside `fleet_pop_share`.

- [ ] **Step 3: Compile-verify (esp32c5, matches the current root sdkconfig)**

Activate ESP-IDF, then from the repo root:

```powershell
idf.py -B build_c5 build
```

Expected: build succeeds (both changed files recompile cleanly into the firmware image).

- [ ] **Step 4: Commit**

```bash
git add main/esp_now_link.c main/coexist.c
git commit -F - <<'EOF'
feat(fleet-census): wire the live census into the Wi-Fi popmatch target

esp_now_link notes each FLEET_MACS sender's real hardware MAC (info->src_addr)
as a live peer node. coexist's Wi-Fi reprofile-tick target now divides by
fleet_pop_live_size(now) instead of the static -DSIMULACRA_FLEET_SIZE, so the
fleet self-configures instead of needing a re-flash when membership changes.
BLE/persona population at boot is UNCHANGED (still static-config; runs before
ESP-NOW starts, so no live census is available at that point -- see spec).

Compile-verified esp32c5. HW validation (actually running >1 node) deferred
-- not attempted overnight per the no-hardware-changes-while-unattended rule.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

## Finishing

After both tasks pass:
- Confirm the full `decoy_audit` suite green (Task 1 Step 10) and firmware compile-verified (Task 2
  Step 3).
- Use **superpowers:finishing-a-development-branch**: merge `feat/dynamic-fleet-census` to `main`
  `--no-ff`; PII-scan the range (`C:\Users`, `/Users/`, `/home/`, the OS username, real MACs); push
  `main`.
- Update `private/PROJECT-MAP.md` §11 and memory with: what shipped, the Wi-Fi-only v1 scope
  boundary, and that HW validation (actually running ≥2 nodes and confirming the target divides
  live) is still open for the user's return.

## Self-Review

**Spec coverage:** peer-node table (add/refresh/TTL/evict, separate from MAC-exclusion table) →
Task 1 Steps 3-4; `fleet_pop_live_size` (N+1, degrades to 1) → Task 1 Steps 5-6; sender-identity
wiring (`info->src_addr`, no self-loopback handling needed) → Task 2 Step 1; single call-site change
at the Wi-Fi reprofile tick, BLE/persona boot path untouched → Task 2 Step 2 + explicitly restated in
the commit message; `FLEET_NODE_CAP=8` / reuse `FLEET_MAC_TTL_MS` → Global Constraints + Task 1 Step
3. All spec sections map to a task.

**Placeholder scan:** no TBD/TODO; every code step is complete.
`test_independent_of_mac_exclusion_table`'s body is a structural `pass` with a comment explaining
*why* no runtime assertion is needed (the two tables are architecturally separate — `--fleetnode`
never calls `fleet_note_peer_macs`) — this is a deliberate documentation-test, not an unfinished one;
still counts toward the "6 tests" total.

**Type consistency:** `fleet_note_peer_node(const uint8_t[6], uint32_t)` /
`fleet_node_count(uint32_t) -> size_t` consistent between `fleet.h` (Task 1 Step 3) and `fleet.c`
(Step 4) and the `--fleetnode` harness (Step 8). `fleet_pop_live_size(uint32_t) -> int` consistent
between `fleet_pop.h` (Step 5), `fleet_pop.c` (Step 6), the harness (Step 8), and the `coexist.c`
call site (Task 2 Step 2). `fleet_pop_share_k(int, int) -> int` is the pre-existing, unchanged
signature — Task 2 Step 2 just supplies a different second argument.
