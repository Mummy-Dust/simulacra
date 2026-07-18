# Fleet-Aware Wi-Fi Population-Match Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Wi-Fi population-match correct across a multi-decoy fleet: decoys exclude each other's probes, and each emits `density / K` so the fleet total matches observed density.

**Architecture:** Three small firmware changes — (A) the fleet self-exclusion broadcast also carries the Wi-Fi probe MACs, (B) the peer-MAC table grows to hold them, (C) the reprofile target is divided by fleet size K via the existing `fleet_pop` module. New logic is glue + a constant; the estimator and RX path are unchanged.

**Tech Stack:** C (ESP-IDF firmware). Compile-verify with `idf.py` (esp32c6). No host-test framework changes (the touched primitives — `fleet_pop_share`, `fleet_macs_pack`, the peer table — are already covered).

## Global Constraints

- Frame budget: `RADAR_FRAME_MAX` = 250; seal overhead = HDR(4)+NONCE(12)+TAG(16) = **32 B**, so payload ≤ **218 B** (≤ 36 MACs). Combined BLE+probe MACs are capped at **`FLEET_BCAST_MACS_MAX` = 32** (193 B payload → 225 B sealed) — a defensive constant so future ceiling bumps can't overflow the seal.
- `FLEET_MAC_CAP` 48 → **96**; `-DSIMULACRA_FLEET_SIZE=K` (already forwarded in `CMakeLists.txt`); deploy value **K=3**.
- All three decoys must run **the same firmware** for mutual exclusion to work.
- **Never hardcode the OS username / forbidden committer name into a tracked file** (`private/TOOLING-GOTCHAS.md`).
- Commit identity `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>`; every commit ends with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.
- Branch `feat/fleet-wifi-popmatch`; merge to `main` `--no-ff`, PII-scan the range, push at the end.

---

## File Structure

- **Modify** `main/fleet.h` — `FLEET_MAC_CAP` 48 → 96; add `FLEET_BCAST_MACS_MAX`.
- **Modify** `main/esp_now_link.c` — `broadcast_fleet_macs` also packs probe MACs (`+#include "probe_agents.h"`).
- **Modify** `main/coexist.c` — wrap the reprofile target in `fleet_pop_share` (`+#include "fleet_pop.h"`).

All three change together to fix one behavior; they are one reviewable unit → one task. Verification is compile (esp32c6) + the existing `churn_selftest` fleet test; the behavioral proof is the HW re-capture (Finishing).

---

### Task 1: Fleet-aware Wi-Fi population-match (broadcast probe MACs + bigger table + /K target)

**Files:**
- Modify: `main/fleet.h` (`FLEET_MAC_CAP`, new `FLEET_BCAST_MACS_MAX`)
- Modify: `main/esp_now_link.c` (`broadcast_fleet_macs` ~lines 204-220; includes)
- Modify: `main/coexist.c` (reprofile target ~lines 271-272; includes)

**Interfaces:**
- Consumes: `probe_agents_count()`, `probe_agents_at(int)->mac` (`probe_agents.h`); `fleet_pop_share(int)` (`fleet_pop.h`); existing `churn_active_count/at`, `fleet_macs_pack`, `radar_wire_seal`.
- Produces: no new symbols; changes the FLEET_MACS broadcast contents, the peer-table size, and the Wi-Fi target sizing.

- [ ] **Step 1: Grow the table + add the broadcast cap in `main/fleet.h`**

Change `FLEET_MAC_CAP` and add `FLEET_BCAST_MACS_MAX`:

```c
#ifndef FLEET_MAC_CAP
#define FLEET_MAC_CAP 96          // peer synthetic MACs tracked (~2 peers x [16 BLE + 16 probe] + headroom)
#endif
#ifndef FLEET_BCAST_MACS_MAX
#define FLEET_BCAST_MACS_MAX 32   // max MACs packed per FLEET_MACS broadcast (fits sealed <=250: 32*6+1=193 + 32 = 225)
#endif
```

(The existing `FLEET_MAC_CAP 48` line is the one to replace.)

- [ ] **Step 2: Pack probe MACs into the broadcast in `main/esp_now_link.c`**

Add the include with the others (near `#include "churn.h"`):

```c
#include "probe_agents.h"
```

Replace the `broadcast_fleet_macs` collection (current lines ~206-211, the `uint8_t macs[...]` decl through the `if (n == 0) return;`) with:

```c
    uint8_t macs[FLEET_BCAST_MACS_MAX][6]; size_t n = 0;
    for (size_t s = 0; s < churn_active_count() && n < FLEET_BCAST_MACS_MAX; s++) {
        const identity_t *id = churn_active_at(s);
        if (id) memcpy(macs[n++], id->addr, 6);
    }
    for (int i = 0; i < probe_agents_count() && n < FLEET_BCAST_MACS_MAX; i++) {
        const probe_agent_t *a = probe_agents_at(i);
        if (a) memcpy(macs[n++], a->mac, 6);        // Wi-Fi probe MACs -> peers exclude them from density
    }
    if (n == 0) return;
```

The rest of `broadcast_fleet_macs` (fleet_key, pack, seal, send) is unchanged — `fleet_macs_pack` and `radar_wire_seal` already handle the larger count, and the sealed frame (≤225 B) fits `RADAR_FRAME_MAX` (250).

- [ ] **Step 3: Divide the reprofile target by K in `main/coexist.c`**

Add the include with the others (near `#include "fleet_pop.h"` neighbors — e.g. after `#include "probe_agents.h"`):

```c
#include "fleet_pop.h"
```

Change the reprofile target update (current lines ~271-272):

```c
            int wt = s_wifi_obs_ok ? wifi_obs_target(now) : WIFI_OBS_FALLBACK;
            probe_agents_set_target(fleet_pop_share(wt), now);       // /K: fleet emits ~density total, not K*density
```

(Only the second line changes — wrap `wt` in `fleet_pop_share`. The `ESP_LOGW("wifi popmatch: ...")` line below it stays; it still logs the pre-division density and now the divided agent count.)

- [ ] **Step 4: Compile-verify (esp32c6)**

Activate ESP-IDF, then from the repo root:

```powershell
. C:\Users\<idf>\esp\v5.5\esp-idf\export.ps1    # machine's IDF export (do NOT hardcode a username in any committed file)
idf.py -B build_c6 build
```

Expected: build succeeds (`esp_now_link.c`, `coexist.c`, `fleet.h` consumers recompile; image generated). Source is target-independent → c5 compiles identically.

- [ ] **Step 5: On-target fleet self-test sanity (optional, host-free)**

The peer-table logic is exercised by the existing `churn_selftest` `test_fleet`; if flashing a SELFTEST build, confirm it still passes (the `FLEET_MAC_CAP` bump doesn't change its assertions — it packs/unpacks 3 MACs and checks TTL). Not required to merge.

- [ ] **Step 6: Commit**

```bash
git add main/fleet.h main/esp_now_link.c main/coexist.c
git commit -F - <<'EOF'
feat(fleet-wifi-popmatch): fleet-aware Wi-Fi population-match

(A) broadcast_fleet_macs also packs probe_agents MACs so decoys exclude each
other's probes from the density count (RX unchanged; sealed frame 225<=250B,
capped at FLEET_BCAST_MACS_MAX=32). (B) FLEET_MAC_CAP 48->96 to hold peers'
BLE+probe MACs. (C) coexist divides the reprofile target by fleet size K via
fleet_pop_share, so K decoys emit ~density total, not K*density.

Compile-verified esp32c6; fleet behavior HW-gated (needs all decoys on this fw
+ -DSIMULACRA_FLEET_SIZE=3 + a re-capture).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

## Finishing

After Task 1 compiles and commits, use **superpowers:finishing-a-development-branch**:
- The `tools/probe_audit` + `tools/decoy_audit` host suites are unaffected — run them to confirm still green (42 / 95).
- Merge `feat/fleet-wifi-popmatch` to `main` `--no-ff`; PII-scan the range (`C:\Users`, `/Users/`, `/home/`, the OS username, real MACs); push `main`.
- Update `private/PROJECT-MAP.md` §11, `private/KISMET-VALIDATION.md`, and memory (fleet-aware fix done; HW re-capture pending).

### Deploy runbook (this session flashes; user re-captures)

1. Build the two decoy targets with `-DSIMULACRA_FLEET_SIZE=3` and the fleet regime the boards already use, then flash each:
   - **C6 (COM13):** `idf.py -B build_c6 -DSIMULACRA_FLEET_SIZE=3 build` then `idf.py -B build_c6 -p COM13 flash` (reconfigure only adds the define; incremental).
   - **C5 (COM12, COM16):** a C5 build (`build_c5`) with `set-target esp32c5` + `-DSIMULACRA_FLEET_SIZE=3`, flashed to each C5 port. (C5 needs the slower set-target the first time.)
   - Chip-detect each port first (`python -m esptool --port COMx chip_id`) — ports drift.
2. Confirm each boots (serial: `wifi observe up (promiscuous on STA)`, probe bursts, and after ~5 min a `wifi popmatch: density=N -> agents=M` with M ≈ N/3).
3. **User re-captures** ~30 min with Kismet (all 3 decoys running). Then re-run
   `tools/probe_audit/probe_behavior_scorecard.py` and the before/after cluster analysis — the decoy
   wildcard-only footprint should shrink toward a realistic crowd (each board ~density/3). Record in
   `private/KISMET-VALIDATION.md`.

## Self-Review

**Spec coverage:** (A) broadcast probe MACs → Task 1 Step 2; (B) FLEET_MAC_CAP 48→96 → Step 1; (C) fleet_pop_share on the target → Step 3; deploy K=3 → Finishing runbook; frame budget cap → Global Constraints + Step 1 `FLEET_BCAST_MACS_MAX`. All spec sections map.

**Placeholder scan:** no TBD/TODO; every code step is complete. Step 4's `<idf>` is the deliberate do-not-hardcode-username instruction, not a code gap.

**Type consistency:** `FLEET_BCAST_MACS_MAX` used consistently in `fleet.h` (def) and `esp_now_link.c` (array size + loop bounds); `probe_agents_count()`/`probe_agents_at(i)->mac` match `probe_agents.h`; `fleet_pop_share(int)->int` matches `fleet_pop.h` and wraps the existing `wt` int. The `macs[FLEET_BCAST_MACS_MAX][6]` array and `fleet_macs_pack(pl, sizeof pl, macs, n)` call are consistent (n ≤ 32 ≤ pack's own clamp).
