# Persona Intra-Life Address Rotation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A bound persona rotates its BLE RPA and its Wi-Fi probe MAC intra-life (~8–15 min jittered, independent per radio), keeping the persona binding, so no persona is trackable by one address for up to 40 min.

**Architecture:** Two independent firmware changes — bound RPA rotation in `ble_devices_tick`, and a per-agent intra-life MAC-rotation timer in `probe_agents`. Each is exercised by a new deterministic harness mode (`synth_dump --blerot`, `probe_dump --agentrot`) that binds one long-lived slot and dumps the address on change, so a host test can assert intra-life rotation with the binding stable.

**Tech Stack:** C (ESP-IDF firmware; MSVC `cl` for host harnesses), Python 3.12 stdlib `unittest`.

## Global Constraints

- Rotation bands: BLE `PERSONA_RPA_ROT_MIN_MS`=**480000** (8 min) / `MAX`=**900000** (15 min); Wi-Fi `PERSONA_MAC_ROT_MIN_MS`/`MAX` = same 8/15 min. Independent draws per radio.
- **Binding by time, not address:** rotation leaves `persona_idx`/`persona_gen`/`born_ms`/`life_ms`/`atype`(BLE)/`arch`(Wi-Fi) untouched — only the address bytes (+ Wi-Fi seq base) change.
- **Law-3:** a bound RPA stays an anonymous phone shape; rotation does not touch `id.payload` — Law-3 result unchanged.
- **Uniqueness:** BLE rotation via `make_random_addr` (→ `uniq_id`), Wi-Fi via `probe_random_mac` (→ `uniq_id`).
- Host tests: `"C:/Program Files/Python312/python.exe" -m pytest -q` from the tool dir; `cl` rebuilds from a "Developer PowerShell for VS". Firmware compile-verify **esp32c6** (`idf.py -B build_c6 build`).
- **Never hardcode the OS username / forbidden committer name into a tracked file** (`private/TOOLING-GOTCHAS.md`).
- Commit identity `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>`; every commit ends with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.
- Branch `feat/persona-intra-life-rotation`; merge to `main` `--no-ff`, PII-scan, push at the end.

---

## File Structure

- **Modify** `main/ble_devices.c` — persona RPA band + helper; rotate bound RPAs in `ble_devices_tick`; seed `next_rotate_ms` from the persona band in `ble_device_sync`.
- **Modify** `main/probe_agents.h` — `probe_agent_t` gains `next_mac_rotate_ms`.
- **Modify** `main/probe_agents.c` — persona MAC band + helper; seed the timer in `agent_spawn`/`probe_agent_sync`; rotate due agents in `probe_agents_lifecycle`.
- **Modify** `tools/decoy_audit/synth_dump.c` — `--blerot` mode; **Test** `tools/decoy_audit/tests/test_ble_rotation.py`.
- **Modify** `tools/probe_audit/probe_dump.c` — `--agentrot` mode; **Test** `tools/probe_audit/tests/test_agent_rotation.py`.

---

### Task 1: BLE RPA intra-life rotation

**Files:**
- Modify: `main/ble_devices.c` (bands ~line 24; helper ~line 52; `ble_devices_tick` rotation loop lines 119-128; `ble_device_sync` line 150)
- Modify: `tools/decoy_audit/synth_dump.c` (new `--blerot` mode, before the `--routecheck` block ~line 203)
- Test: `tools/decoy_audit/tests/test_ble_rotation.py`

**Interfaces:**
- Produces: `synth_dump --blerot <seed> <ticks> <tickms>` → lines `<t_ms> <addr_hex12> <persona_gen>`, one per address change of a single bound slot (40-min life, gen 1).

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_ble_rotation.py`:

```python
import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def blerot(seed=1, ticks=45, tickms=60000):
    out = subprocess.check_output([EXE, "--blerot", str(seed), str(ticks), str(tickms)], text=True)
    return [(int(t), a, int(g)) for t, a, g in (ln.split() for ln in out.splitlines())]


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class BleRotation(unittest.TestCase):
    def test_bound_rpa_rotates_intra_life(self):
        rows = blerot()                                   # 45 min, one bound slot (life 40min, gen 1)
        addrs = [a for _, a, _ in rows]
        self.assertGreaterEqual(len(rows), 3)             # initial addr + >=2 rotations
        self.assertEqual(len(addrs), len(set(addrs)))     # every rotation a fresh unique address
        self.assertEqual({g for _, _, g in rows}, {1})    # intra-life: generation never changed

    def test_rotation_spacing_in_band(self):
        times = [t for t, _, _ in blerot()]
        for a, b in zip(times, times[1:]):
            self.assertTrue(480000 <= b - a < 960000, f"gap {b-a} ms outside 8-15min (+1 tick)")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_ble_rotation.py -q`
Expected: FAIL — no `--blerot` mode, `blerot()` gets non-conforming output → `ValueError`/empty.

- [ ] **Step 3: Add the persona RPA band + helper in `main/ble_devices.c`**

After the existing `RPA_ROT_*` / `NRPA_ROT_*` defines (~line 25):

```c
// Bound-persona RPA rotates on the fast-realistic end (real phones ~15 min), shorter than the unbound
// RPA_ROT band, so a persona is never trackable by one address for its whole (up to 40 min) life.
#define PERSONA_RPA_ROT_MIN_MS   480000u    // 8 min
#define PERSONA_RPA_ROT_MAX_MS   900000u    // 15 min
```

After `rotate_base` (~line 52):

```c
static uint32_t persona_rpa_rotate_base(void) { return rnd_range(PERSONA_RPA_ROT_MIN_MS, PERSONA_RPA_ROT_MAX_MS); }
```

- [ ] **Step 4: Rotate bound RPAs in `ble_devices_tick`**

Replace the rotation loop (lines 119-128) with (drops the bound-skip; bound reschedules on the persona band):

```c
    for (int i = 0; i < s_n; i++) {
        ble_device_t *d = &s_dev[i];
        if (!d->alive) continue;
        if (d->atype == BLE_ATYPE_STATIC) continue;        // static never rotates (bound are always RPA)
        if ((int32_t)(now_ms - d->next_rotate_ms) >= 0) {
            make_random_addr(d->id.addr, top2_for(d->atype));   // fresh unique addr; binding untouched
            d->next_rotate_ms = now_ms + (d->persona_idx >= 0 ? persona_rpa_rotate_base()
                                                              : rotate_base(d->atype));
        }
    }
```

- [ ] **Step 5: Seed the bound timer from the persona band in `ble_device_sync`**

Change line 150 (`d->next_rotate_ms = born_ms + rotate_base(BLE_ATYPE_RPA);`) to:

```c
    d->next_rotate_ms = born_ms + persona_rpa_rotate_base();   // bound: fast persona band
```

- [ ] **Step 6: Add the `--blerot` harness mode to `tools/decoy_audit/synth_dump.c`**

Insert before the `--routecheck` block (~line 203):

```c
    if (argc > 1 && strcmp(argv[1], "--blerot") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ticks  = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 45;
        unsigned tickms = argc > 4 ? (unsigned)strtoul(argv[4], 0, 10) : 60000;   // 1 min ticks
        srand(seed);
        roster_init();
        uint32_t t = 0;
        ble_devices_init(1, t);
        ble_device_sync(0, 0, 0, t, 2400000u, 1);          // bound slot 0, 40 min life, gen 1
        char last[13] = "";
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            ble_devices_tick(t);
            const ble_device_t *d = ble_devices_at(0);
            char hex[13]; for (int b = 0; b < 6; b++) sprintf(hex + b * 2, "%02x", d->id.addr[b]);
            if (strcmp(hex, last) != 0) { printf("%u %s %u\n", (unsigned)t, hex, (unsigned)d->persona_gen); strcpy(last, hex); }
        }
        return 0;
    }
```

- [ ] **Step 7: Rebuild `synth_dump.exe`**

From a Developer PowerShell for VS, in `tools/decoy_audit`, run the `cl` line from `README.md` (the one that includes `ble_devices.c`, `roster.c`, `templates.c`, `uniq_id.c`, `wifi_density.c`, …). Equivalent: `.\run.ps1 -Rebuild`. Expected: clean build.

- [ ] **Step 8: Run the test to verify it passes**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_ble_rotation.py -q`
Expected: PASS (2 tests). Then the full suite `-q` → all pass (was 95; now 97).

- [ ] **Step 9: Commit**

```bash
git add main/ble_devices.c tools/decoy_audit/synth_dump.c tools/decoy_audit/tests/test_ble_rotation.py
git commit -F - <<'EOF'
feat(persona-rotation): bound BLE RPA rotates intra-life (~8-15 min)

ble_devices_tick now rotates a bound persona's RPA on a fast persona band
(PERSONA_RPA_ROT 8-15 min) instead of holding one address for the whole
(up to 40 min) persona life. Binding (persona_idx/gen/born/life) and Law-3
payload untouched; fresh unique addr via uniq_id. Host-tested via
synth_dump --blerot.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 2: Wi-Fi probe MAC intra-life rotation

**Files:**
- Modify: `main/probe_agents.h` (`probe_agent_t` struct)
- Modify: `main/probe_agents.c` (band + helper ~line 11; `agent_spawn` ~line 28; `probe_agent_sync` ~line 104; `probe_agents_lifecycle` ~line 49)
- Modify: `tools/probe_audit/probe_dump.c` (new `--agentrot` mode)
- Test: `tools/probe_audit/tests/test_agent_rotation.py`

**Interfaces:**
- Consumes: nothing from Task 1 (independent).
- Produces: `probe_dump --agentrot <seed> <ticks> <tickms>` → lines `<t_ms> <mac_hex12> <persona_gen>`, one per MAC change of a single bound agent (40-min life, gen 1).

- [ ] **Step 1: Write the failing test**

Create `tools/probe_audit/tests/test_agent_rotation.py`:

```python
import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def agentrot(seed=1, ticks=35, tickms=60000):
    out = subprocess.check_output([EXE, "--agentrot", str(seed), str(ticks), str(tickms)], text=True)
    return [(int(t), m, int(g)) for t, m, g in (ln.split() for ln in out.splitlines())]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class AgentRotation(unittest.TestCase):
    def test_bound_mac_rotates_intra_life(self):
        rows = agentrot()                                 # 35 min < 40 min life -> no reincarnation
        macs = [m for _, m, _ in rows]
        self.assertGreaterEqual(len(rows), 2)             # initial MAC + >=1 rotation
        self.assertEqual(len(macs), len(set(macs)))       # each rotation a fresh unique MAC
        self.assertEqual({g for _, _, g in rows}, {1})    # intra-life: generation unchanged

    def test_spacing_in_band(self):
        times = [t for t, _, _ in agentrot()]
        for a, b in zip(times, times[1:]):
            self.assertTrue(480000 <= b - a < 960000, f"gap {b-a} ms outside 8-15min (+1 tick)")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_agent_rotation.py -q`
Expected: FAIL — no `--agentrot` mode.

- [ ] **Step 3: Add the field to `probe_agent_t` in `main/probe_agents.h`**

Add after `uint32_t next_scan_ms;`:

```c
    uint32_t     next_mac_rotate_ms;   // intra-life Wi-Fi MAC rotation (independent of the BLE RPA timer)
```

- [ ] **Step 4: Add the persona MAC band + helper + seed it in `main/probe_agents.c`**

After the existing `IDLE_*` defines (~line 10):

```c
#define PERSONA_MAC_ROT_MIN_MS 480000u   // 8 min  (Wi-Fi MAC intra-life rotation, fast-realistic)
#define PERSONA_MAC_ROT_MAX_MS 900000u   // 15 min
```

After `rnd_range` (~line 15):

```c
static uint32_t persona_mac_rotate_base(void) { return rnd_range(PERSONA_MAC_ROT_MIN_MS, PERSONA_MAC_ROT_MAX_MS); }
```

At the end of `agent_spawn` (after `a->next_scan_ms = ...;`, ~line 28) and at the end of `probe_agent_sync` (after `a->next_scan_ms = ...;`, ~line 104), add — using each function's own `now_ms`/`born_ms` base:

```c
    a->next_mac_rotate_ms = now_ms + persona_mac_rotate_base();    // agent_spawn: base now_ms
```
```c
    a->next_mac_rotate_ms = born_ms + persona_mac_rotate_base();   // probe_agent_sync: base born_ms
```

- [ ] **Step 5: Rotate due agents in `probe_agents_lifecycle`**

Replace the loop body in `probe_agents_lifecycle` (currently reincarnation only, ~lines 51-55) with:

```c
    for (int i = 0; i < s_n; i++) {
        probe_agent_t *a = &s_agents[i];
        if (a->alive && (now_ms - a->born_ms) >= a->life_ms) { agent_spawn(a, now_ms); reborn++; continue; }
        // intra-life MAC rotation: a real phone rotates its Wi-Fi MAC within a session, independent of
        // the BLE RPA. Fresh privacy identity; keeps arch/duty/born/life/persona_gen (the binding).
        if (a->alive && (int32_t)(now_ms - a->next_mac_rotate_ms) >= 0) {
            probe_random_mac(a->mac);
            a->seq = (uint16_t)(esp_random() & 0x0FFFu);
            a->next_mac_rotate_ms = now_ms + persona_mac_rotate_base();
        }
    }
```

(`probe_random_mac` is already used by `agent_spawn`; it's declared in `probe_frame.h`, already included.)

- [ ] **Step 6: Add the `--agentrot` harness mode to `tools/probe_audit/probe_dump.c`**

Insert at the start of `main()` (near the other new modes):

```c
    if (argc > 1 && strcmp(argv[1], "--agentrot") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      ticks  = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 35;
        unsigned tickms = argc > 4 ? (unsigned)strtoul(argv[4], 0, 10) : 60000;
        srand(seed);
        probe_agents_init(1, 0);
        probe_agent_sync(0, ARCH_IPHONE, 0, 2400000u, 1);   // bound agent, 40 min life, gen 1
        char last[13] = "";
        uint32_t t = 0;
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            probe_agents_lifecycle(t);
            const probe_agent_t *a = probe_agents_at(0);
            char hex[13]; for (int b = 0; b < 6; b++) sprintf(hex + b * 2, "%02x", a->mac[b]);
            if (strcmp(hex, last) != 0) { printf("%u %s %u\n", (unsigned)t, hex, (unsigned)a->persona_gen); strcpy(last, hex); }
        }
        return 0;
    }
```

- [ ] **Step 7: Rebuild `probe_dump.exe`**

From a Developer PowerShell for VS, in `tools/probe_audit`, run the `cl` line from Task 1 of the earlier fleet plan / `run.ps1` (`probe_dump.c … probe_agents.c … wifi_density.c`). Expected: clean build.

- [ ] **Step 8: Run the test to verify it passes**

Run: `cd tools/probe_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_agent_rotation.py -q`
Expected: PASS (2 tests). Then the full suite `-q` → all pass (was 42; now 44). Also re-run the existing `test_probe_agents.py` to confirm the lifecycle change didn't regress the seq/turnover tests.

- [ ] **Step 9: Commit**

```bash
git add main/probe_agents.h main/probe_agents.c tools/probe_audit/probe_dump.c tools/probe_audit/tests/test_agent_rotation.py
git commit -F - <<'EOF'
feat(persona-rotation): bound Wi-Fi probe MAC rotates intra-life (~8-15 min)

probe_agents_lifecycle now rotates a bound agent's MAC (+ seq base) on a fast
persona band (PERSONA_MAC_ROT 8-15 min), independent of the BLE RPA timer,
instead of holding one MAC for the whole persona life. Keeps
arch/duty/born/life/persona_gen (binding). Host-tested via probe_dump --agentrot.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

## Finishing

After both tasks' host tests pass:
- **Compile-verify esp32c6** (covers both firmware changes): activate IDF, `idf.py -B build_c6 build` → expect success.
- Use **superpowers:finishing-a-development-branch**: confirm the full `decoy_audit` (97) and `probe_audit` (44) suites are green; merge `feat/persona-intra-life-rotation` to `main` `--no-ff`; PII-scan the range; push `main`.
- Update `private/PROJECT-MAP.md` §11 and memory (`cross-protocol-personas`): intra-life RPA/MAC rotation done; the persona follow-up "intra-life RPA rotation" is closed.

## Self-Review

**Spec coverage:** BLE RPA rotation (band, tick rotate, sync seed) → Task 1; Wi-Fi MAC rotation (struct field, band, seed, lifecycle rotate) → Task 2; independence (separate timers/draws) → both; binding-by-time preserved (untouched fields) → both, asserted by the `{gen} == {1}` tests; Law-3 (payload untouched) → Task 1 note; uniqueness (`make_random_addr`/`probe_random_mac`) → both, implied by the distinct-address assertions; presence-axis improvement → the honest target, exercised on-device not merge-gating. All spec sections map.

**Placeholder scan:** no TBD/TODO; every code step complete. The `cl` rebuild steps reference the existing README/run.ps1 source lists rather than repeating them — those are stable and already carry the needed sources.

**Type consistency:** `PERSONA_RPA_ROT_MIN/MAX_MS` + `persona_rpa_rotate_base()` (ble_devices.c) and `PERSONA_MAC_ROT_MIN/MAX_MS` + `persona_mac_rotate_base()` (probe_agents.c) are each defined and used within their own file. `next_mac_rotate_ms` is added to `probe_agent_t` (Task 2 Step 3) and used in `agent_spawn`/`probe_agent_sync`/`probe_agents_lifecycle` (Steps 4-5). Harness output format `<t> <hex12> <gen>` matches both tests' 3-field split. `ble_device_sync(0,0,0,t,2400000u,1)` matches its signature `(int slot,int persona_idx,bool apple,uint32,uint32,uint32)`.
