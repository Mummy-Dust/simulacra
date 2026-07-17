# Mesh-Distributed Persona Population (M10 v2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let each decoy node run `1/K` of the fleet-wide population (`K` = static fleet size) so `K` physically separated nodes spread the decoy crowd across `K` points, and quantify the win with a K-node RSSI audit model.

**Architecture:** One new dependency-free pure module `main/fleet_pop.{h,c}` provides `fleet_pop_share(target)` = `round(target/K)`; it is applied at the single population-sizing chokepoint in `simulacra_main.c` (the model target, the fallback default, the BLE floor, and the persona count). `K` arrives as a compile-time `-DSIMULACRA_FLEET_SIZE` (default 1 → identity → standalone unchanged). The host audit (`tools/decoy_audit/`) gains a K-node modeled-RSSI variant that draws each node's base position from the real capture's own RSSI distribution (anchored, no free parameter) and reports separability vs `K`.

**Tech Stack:** C (ESP-IDF firmware, MSVC `cl` for host tests), Python 3.12 stdlib `unittest` (run via pytest). No new dependencies.

## Global Constraints

- `-DSIMULACRA_FLEET_SIZE=K`, **default 1**; at `K=1` all population sizing is byte-identical to today (only a log string gains `fleet_k=1`).
- `fleet_pop_share(target)` = `max(1, round(target/K))` for `target>0`; `target<=0` or `k<=1` returns `target`. A node never drops a whole population class to zero.
- **Composition stays i.i.d. per node** — same family/vendor/address-type/interval distributions, just fewer identities. **No per-node index. No family partitioning. No radio-splitting.**
- The audit K-node model is **placement-invariant** (median-centered, via existing `rssi_separability`) and **anchored** to the real profile's `rssi_bins` (no free spread parameter). `K=1` must reproduce the current single-node `d_rssi` value **exactly**.
- **Honest ceiling — "K points, not N; the win is proportional to node count"** — must survive into every doc/roadmap edit.
- Commit identity `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>`; every commit ends with the trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.
- Branch: `feat/mesh-distributed-personas`. Merge to `main` locally `--no-ff`; **PII-scan** the diff (`C:\Users`, `/Users/`, `/home/`, the OS username, real MACs) before pushing.
- Audit tests run from `tools/decoy_audit/` via `"C:/Program Files/Python312/python.exe" -m pytest -q`.
- Host builds have **MSVC only** (no gcc); build `synth_dump.exe` with the `cl` line shown in Task 1, not `make`.

---

## File Structure

- **Create** `main/fleet_pop.h` — the fleet-size + share interface (pure).
- **Create** `main/fleet_pop.c` — the implementation.
- **Modify** `tools/decoy_audit/synth_dump.c` — add `--fleet-share` / `--fleet-size` host-test modes.
- **Modify** `tools/decoy_audit/Makefile`, `tools/decoy_audit/run.ps1`, `tools/decoy_audit/README.md` — add `fleet_pop.c` to the build.
- **Create** `tools/decoy_audit/tests/test_fleet_pop.py` — host unit test for the arithmetic.
- **Modify** `main/CMakeLists.txt` — add `fleet_pop.c` to SRCS and `SIMULACRA_FLEET_SIZE` to the flag forwarder.
- **Modify** `main/simulacra_main.c` (≈ lines 140–158) — apply `fleet_pop_share` at the sizing chokepoint.
- **Modify** `tools/decoy_audit/discriminators.py` — add the K-node modeled RSSI + `d_rssi_k`.
- **Create** `tools/decoy_audit/tests/test_discriminators_rssi_fleet.py` — tests for the K-node model.
- **Modify** `tools/decoy_audit/scorecard.py` — add a `--fleet-curve K` affordance printing separability vs node count.
- **Modify** `tools/decoy_audit/tests/test_scorecard.py` — test the curve.
- **Modify** `docs/ROADMAP.md` — mark mesh v2 landed with the measured curve; keep the honest ceiling.

---

### Task 1: `fleet_pop` module + host unit test

**Files:**
- Create: `main/fleet_pop.h`
- Create: `main/fleet_pop.c`
- Modify: `tools/decoy_audit/synth_dump.c` (add modes near the top of `main()`, ~line 108)
- Modify: `tools/decoy_audit/Makefile:3-6`, `tools/decoy_audit/run.ps1:60-63`, `tools/decoy_audit/README.md:40-43`
- Test: `tools/decoy_audit/tests/test_fleet_pop.py`

**Interfaces:**
- Produces: `int fleet_pop_size(void)`; `int fleet_pop_share_k(int target, int k)`; `int fleet_pop_share(int target)` (in `main/fleet_pop.h`). Task 2 consumes `fleet_pop_size` and `fleet_pop_share`.

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_fleet_pop.py`:

```python
import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def share(target, k):
    out = subprocess.check_output([EXE, "--fleet-share", str(target), str(k)], text=True)
    return int(out.strip())


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class FleetPop(unittest.TestCase):
    def test_k1_is_identity(self):
        for t in (0, 1, 8, 16, 255):
            self.assertEqual(share(t, 1), t)

    def test_k2_halves_rounded(self):
        self.assertEqual(share(16, 2), 8)
        self.assertEqual(share(15, 2), 8)   # 7.5 -> 8 (round to nearest)
        self.assertEqual(share(1, 2), 1)    # floored, never 0

    def test_k_ge_target_floors_at_one(self):
        self.assertEqual(share(3, 8), 1)
        self.assertEqual(share(1, 5), 1)

    def test_shares_sum_close_to_target(self):
        # K nodes each running round(target/K) sum to within K of the target (rounding slack).
        for t, k in ((24, 3), (16, 2), (30, 4)):
            self.assertAlmostEqual(share(t, k) * k, t, delta=k)

    def test_fleet_size_default_is_one(self):
        # host build passes no -DSIMULACRA_FLEET_SIZE -> #ifndef default 1
        out = subprocess.check_output([EXE, "--fleet-size"], text=True).strip()
        self.assertEqual(out, "1")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_fleet_pop.py -q`
Expected: FAIL — the `--fleet-share` mode does not exist yet, so `int(out.strip())` raises `ValueError` (synth_dump prints the default population instead of a bare integer).

- [ ] **Step 3: Create `main/fleet_pop.h`**

```c
#pragma once
// Fleet population share. In a mesh of K decoy nodes sharing one environment, each node runs 1/K of
// the fleet-wide population so the aggregate matches observed density (design law 4: population-match)
// and the crowd originates from K physical points instead of one. K is static: -DSIMULACRA_FLEET_SIZE=K,
// default 1 (standalone). Pure: no radio, no timers -> host-unit-tested like the other pure cores.

#ifndef SIMULACRA_FLEET_SIZE
#define SIMULACRA_FLEET_SIZE 1
#endif

// Fleet size K, clamped to >= 1 so the divisor is always safe (a bad/zero config becomes 1).
int fleet_pop_size(void);

// round(target / k), floored at 1 for target > 0 (a node never zeroes a whole population class).
// Pure in k -> testable at any k without recompiling. target <= 0 or k <= 1 returns target unchanged.
int fleet_pop_share_k(int target, int k);

// This node's share of a fleet-wide target: fleet_pop_share_k(target, fleet_pop_size()).
int fleet_pop_share(int target);
```

- [ ] **Step 4: Create `main/fleet_pop.c`**

```c
#include "fleet_pop.h"

int fleet_pop_size(void)
{
    int k = SIMULACRA_FLEET_SIZE;
    return k < 1 ? 1 : k;
}

int fleet_pop_share_k(int target, int k)
{
    if (target <= 0 || k <= 1) return target;
    int s = (target + k / 2) / k;   // round to nearest
    return s < 1 ? 1 : s;           // never drop a whole population class to zero
}

int fleet_pop_share(int target)
{
    return fleet_pop_share_k(target, fleet_pop_size());
}
```

- [ ] **Step 5: Add the host-test modes to `tools/decoy_audit/synth_dump.c`**

Add the include near the other `main/` includes at the top of the file (after `#include "rf_model.h"` on line 4):

```c
#include "fleet_pop.h"
```

Insert these two mode handlers at the very start of `main()` (immediately after the `int main(int argc, char **argv) {` line, before the existing `--persona-pop` block):

```c
    if (argc > 1 && strcmp(argv[1], "--fleet-share") == 0) {
        int target = argc > 2 ? (int)strtol(argv[2], 0, 10) : 0;
        int k      = argc > 3 ? (int)strtol(argv[3], 0, 10) : 1;
        printf("%d\n", fleet_pop_share_k(target, k));
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--fleet-size") == 0) {
        printf("%d\n", fleet_pop_size());
        return 0;
    }
```

- [ ] **Step 6: Add `fleet_pop.c` to the three build definitions**

In `tools/decoy_audit/Makefile`, append `$(ROOT)/main/fleet_pop.c` to the `SRC :=` list (the line ending in `probe_frame.c`):

```make
       $(ROOT)/main/uniq_id.c $(ROOT)/main/phantom.c $(ROOT)/main/probe_agents.c $(ROOT)/main/probe_frame.c \
       $(ROOT)/main/fleet_pop.c
```

In `tools/decoy_audit/run.ps1` (the `cl` invocation, ~lines 60-63), add `..\..\main\fleet_pop.c` to the source list, e.g. on the `uniq_id.c ... phantom.c ...` line:

```powershell
           ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c `
           ..\..\main\fleet_pop.c `
```

In `tools/decoy_audit/README.md` (the documented `cl` line, ~lines 40-43), add `..\..\main\fleet_pop.c ^` to the source list in the same place so the doc stays runnable.

- [ ] **Step 7: Rebuild `synth_dump.exe`**

From a Developer PowerShell for VS (so `cl` is on PATH), in `tools/decoy_audit`:

```powershell
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h `
   /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar `
   synth_dump.c ble_hs_adv.c roster_stub.c `
   ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c `
   ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c `
   ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c `
   ..\..\main\fleet_pop.c `
   /Fe:synth_dump.exe
```

Expected: compiles clean, produces `synth_dump.exe`. (Equivalent: `.\run.ps1 -Rebuild`, which rebuilds then runs a full audit.)

- [ ] **Step 8: Run the test to verify it passes**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_fleet_pop.py -q`
Expected: PASS (5 tests). Also run the full suite to confirm no regression:
`"C:/Program Files/Python312/python.exe" -m pytest -q` → Expected: all pass (was 82; now 87).

- [ ] **Step 9: Commit**

```bash
git add main/fleet_pop.h main/fleet_pop.c tools/decoy_audit/synth_dump.c \
        tools/decoy_audit/Makefile tools/decoy_audit/run.ps1 tools/decoy_audit/README.md \
        tools/decoy_audit/tests/test_fleet_pop.py
git commit -F - <<'EOF'
feat(fleet_pop): pure fleet-population-share core + host test

fleet_pop_share(target)=round(target/K) with K from -DSIMULACRA_FLEET_SIZE
(default 1 = identity). Host-tested via synth_dump --fleet-share/--fleet-size.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 2: Apply the share at the firmware sizing chokepoint

**Files:**
- Modify: `main/CMakeLists.txt:2` (SRCS) and `:12-19` (flag forwarder)
- Modify: `main/simulacra_main.c:140-158` (the combined-coexist decoy sizing block)

**Interfaces:**
- Consumes: `fleet_pop_size()`, `fleet_pop_share(int)` from `main/fleet_pop.h` (Task 1).
- Produces: no new symbols; changes runtime population sizing only.

**Note on verification:** `simulacra_main.c` is not host-compiled (it pulls in ESP-IDF), so this task has no host unit test — the risky arithmetic is already covered in Task 1. Verification is compile-verify + an on-target population-log check (below). This is expected for a wiring task.

- [ ] **Step 1: Add `fleet_pop.c` to the firmware build and forward the flag**

In `main/CMakeLists.txt`, append `"fleet_pop.c"` to the end of the `SRCS` list (after `"phantom.c"`):

```cmake
    SRCS "simulacra_main.c" ... "uniq_id.c" "phantom.c" "fleet_pop.c"
```

Add `SIMULACRA_FLEET_SIZE` to the `foreach(flag ...)` list (append to the last line before the closing `)`):

```cmake
    PROBE_FIX_CH PROBE_FORCE_SHARED SNIFF_FIXED_CH
    OBSERVE_LOG_RSSI SIMULACRA_FLEET_SIZE)
```

- [ ] **Step 2: Add the include to `main/simulacra_main.c`**

Near the other `main/` includes (e.g. after `#include "churn_adv.h"`):

```c
#include "fleet_pop.h"
```

- [ ] **Step 3: Apply `fleet_pop_share` at the sizing block**

Replace the current block (`main/simulacra_main.c`, ≈ lines 141-158):

```c
    int ndev = 12;
    {
        rf_model_t m;
        if (rf_model_load_nvs(&m) == 0 && m.total_obs >= GEN_MIN_OBS) {
            uint8_t at = generate_active_target(&m);
            churn_set_active_target(at);
            ndev = (int)at;
            ESP_LOGW(TAG, "population-match: pop=%u active_target=%u",
                     (unsigned)(m.pop_ewma + 0.5f), (unsigned)at);
        }
    }
    if (ndev < probe_desired_ble_floor()) ndev = probe_desired_ble_floor();   // room for personas
    ble_devices_init(ndev, (uint32_t)(esp_timer_get_time() / 1000));  // population size; clamped to max
```

with (each fleet-wide target wrapped in `fleet_pop_share`; `churn_set_active_target` is a no-op today but kept for structure):

```c
    int fleet_k = fleet_pop_size();                 // K nodes share the crowd (default 1 = standalone)
    int ndev = fleet_pop_share(12);                 // fallback density -> this node's share
    {
        rf_model_t m;
        if (rf_model_load_nvs(&m) == 0 && m.total_obs >= GEN_MIN_OBS) {
            uint8_t at = (uint8_t)fleet_pop_share(generate_active_target(&m));  // node's share of observed density
            churn_set_active_target(at);
            ndev = (int)at;
            ESP_LOGW(TAG, "population-match: pop=%u fleet_k=%d active_target=%u",
                     (unsigned)(m.pop_ewma + 0.5f), fleet_k, (unsigned)at);
        }
    }
    int ble_floor = fleet_pop_share(probe_desired_ble_floor());   // floor scales with the persona share
    if (ndev < ble_floor) ndev = ble_floor;                      // room for this node's personas + twins
    ble_devices_init(ndev, (uint32_t)(esp_timer_get_time() / 1000));  // population size; clamped to max
```

Then scale the persona count at the `phantom_init` call (≈ line 158):

```c
    phantom_init(fleet_pop_share(probe_phone_target()), (uint32_t)(esp_timer_get_time() / 1000));
```

- [ ] **Step 4: Compile-verify both decoy targets**

Activate ESP-IDF (`. $env:IDF_PATH\export.ps1` or the project's `export.ps1`), then from the repo root:

```powershell
idf.py set-target esp32c5
idf.py -DSIMULACRA_ESPNOW=1 build
idf.py fullclean; idf.py set-target esp32c6
idf.py -DSIMULACRA_ESPNOW=1 build
```

Expected: both builds succeed. (A default build passes no `-DSIMULACRA_FLEET_SIZE`, so `#ifndef` gives `K=1`.)

- [ ] **Step 5: On-target sanity (hardware; user-driven, record the result)**

Flash one node with `-DSIMULACRA_FLEET_SIZE=2` and confirm the boot log shows `fleet_k=2` and an `active_target` about half the single-node value; flash a standalone build (no flag) and confirm `fleet_k=1` with the original target. Record the two log lines in the commit or the PR notes. If hardware is unavailable at implementation time, note it as deferred — the compile-verify + Task 1 arithmetic tests gate the merge.

- [ ] **Step 6: Commit**

```bash
git add main/CMakeLists.txt main/simulacra_main.c
git commit -F - <<'EOF'
feat(mesh): each node runs 1/K of the population (fleet distribution)

Apply fleet_pop_share to the sizing chokepoint: the model-driven active
target, the no-model fallback, the BLE floor, and the persona count all scale
by 1/K (-DSIMULACRA_FLEET_SIZE). K=1 is byte-identical (log gains fleet_k).
K nodes now place the crowd at K points instead of one.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 3: K-node modeled-RSSI audit discriminator

**Files:**
- Modify: `tools/decoy_audit/discriminators.py` (add after the existing `d_rssi`, ~line 104)
- Test: `tools/decoy_audit/tests/test_discriminators_rssi_fleet.py`

**Interfaces:**
- Consumes: existing `cp.RSSI_LO`, `cp.RSSI_W`, `cp.RSSI_NBINS`, `cp.rssi_hist`, and `rssi_separability`, `modeled_decoy_rssi_hist`, `RSSI_MODEL_SEED`, `RSSI_SIGMA_DB` (all in `discriminators.py`); `random` (already imported, line 6).
- Produces: `modeled_decoy_rssi_hist_k(synth, profile, k, seed=RSSI_MODEL_SEED, sigma=RSSI_SIGMA_DB)`; `d_rssi_k(synth, profile, k)`. Task 4 consumes `d_rssi_k`.

- [ ] **Step 1: Write the failing test**

Create `tools/decoy_audit/tests/test_discriminators_rssi_fleet.py`:

```python
import os, sys, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE); sys.path.insert(0, TOOL)
import capture_profile as cp          # noqa: E402
import discriminators as D            # noqa: E402


def _profile():
    # a broad real crowd (spatially spread) so the single-node decoy is separable
    real = cp.rssi_hist(list(range(-95, -35)))
    return {"rssi_bins": real["rssi_bins"], "rssi_median": real["rssi_median"]}


def _synth(n=256):
    # decoys carry a tx-dither field, like synth_dump rows
    dither = [-12, -9, -6, -3, 0, 3]
    return [{"tx": dither[i % len(dither)]} for i in range(n)]


class RssiFleet(unittest.TestCase):
    def test_k1_reproduces_single_node(self):
        synth, prof = _synth(), _profile()
        self.assertEqual(D.d_rssi_k(synth, prof, 1), D.d_rssi(synth, prof))

    def test_more_nodes_lower_separability(self):
        synth, prof = _synth(), _profile()
        d1 = D.d_rssi_k(synth, prof, 1)
        d8 = D.d_rssi_k(synth, prof, 8)
        self.assertLess(d8, d1)                     # spreading the crowd across nodes helps
        self.assertLess(d8, d1 - 0.02)              # by a clear margin, not just noise

    def test_monotonic_non_increasing(self):
        synth, prof = _synth(), _profile()
        vals = [D.d_rssi_k(synth, prof, k) for k in (1, 2, 3, 4, 6, 8)]
        for a, b in zip(vals, vals[1:]):
            self.assertLessEqual(b, a + 1e-9)       # non-increasing (tolerance for equal shapes)

    def test_bases_drawn_from_real_bins(self):
        import random
        centers = [cp.RSSI_LO + (i + 0.5) * cp.RSSI_W for i in range(cp.RSSI_NBINS)]
        # a real dist concentrated in one bin -> every sampled base is that bin's center
        bins = [0.0] * cp.RSSI_NBINS
        bins[5] = 1.0
        got = D._sample_bases_from_bins(bins, 20, random.Random(1))
        self.assertTrue(all(b == centers[5] for b in got))

    def test_missing_profile_rssi_returns_zero(self):
        self.assertEqual(D.d_rssi_k(_synth(), {}, 4), 0.0)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_discriminators_rssi_fleet.py -q`
Expected: FAIL — `AttributeError: module 'discriminators' has no attribute 'd_rssi_k'`.

- [ ] **Step 3: Implement the K-node model in `tools/decoy_audit/discriminators.py`**

Add directly after the existing `d_rssi` function (after ~line 104):

```python
# --- K-node (mesh) modeled RSSI ---------------------------------------------------------------
# With K separated nodes, decoys originate from K points, not one. We model that by assigning each
# decoy round-robin to a node and giving each node a base RSSI drawn from the REAL crowd's own
# distribution (anchored -> no free spread parameter). k=1 delegates to the single-node model so the
# score is unchanged. As k grows the crowd spread approaches the real crowd's -> separability falls.
def _sample_bases_from_bins(rssi_bins, k, rng):
    """Draw k node-base RSSIs (dBm bin-centers) from a normalized histogram, weighted by the bins."""
    centers = [cp.RSSI_LO + (i + 0.5) * cp.RSSI_W for i in range(cp.RSSI_NBINS)]
    weights = rssi_bins if any(rssi_bins) else [1.0] * len(centers)
    return [rng.choices(centers, weights=weights, k=1)[0] for _ in range(k)]


def modeled_decoy_rssi_hist_k(synth, profile, k, seed=RSSI_MODEL_SEED, sigma=RSSI_SIGMA_DB):
    """K-node modeled decoy RSSI. k<=1 (or no real RSSI) delegates to the single-node model so k=1
    reproduces d_rssi exactly. For k>1, node bases are drawn from a SEPARATE rng stream so the jitter
    sequence is identical to the single-node model."""
    real_bins = profile.get("rssi_bins")
    if k <= 1 or not real_bins:
        return modeled_decoy_rssi_hist(synth, seed=seed, sigma=sigma)
    bases = _sample_bases_from_bins(real_bins, k, random.Random(seed ^ 0x9E3779B9))
    jit = random.Random(seed)
    samples = []
    i = 0
    for x in synth:
        tx = x.get("tx")
        if tx is None:
            continue
        samples.append(bases[i % k] + tx + jit.gauss(0.0, sigma))
        i += 1
    return cp.rssi_hist(samples)


def d_rssi_k(synth, profile, k):
    """Separability of the K-node modeled decoy RSSI from the real crowd (placement-invariant).
    d_rssi_k(.,.,1) == d_rssi(.,.). 0.0 when the profile has no RSSI."""
    real_bins = profile.get("rssi_bins")
    if not real_bins:
        return 0.0
    decoy = modeled_decoy_rssi_hist_k(synth, profile, k)
    if not decoy:
        return 0.0
    return rssi_separability((decoy["rssi_bins"], decoy["rssi_median"]),
                             (real_bins, profile.get("rssi_median", 0.0)))
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_discriminators_rssi_fleet.py -q`
Expected: PASS (5 tests). Confirm `d_rssi_k(.,.,1) == d_rssi(.,.)` holds (the k<=1 delegation guarantees it).

- [ ] **Step 5: Commit**

```bash
git add tools/decoy_audit/discriminators.py tools/decoy_audit/tests/test_discriminators_rssi_fleet.py
git commit -F - <<'EOF'
feat(decoy_audit): K-node modeled RSSI (separability vs node count)

modeled_decoy_rssi_hist_k assigns decoys round-robin to K nodes, each node's
base RSSI drawn from the real capture's own distribution (anchored, no free
param). k=1 delegates to the single-node model (score unchanged); larger k
spreads the crowd across K points -> lower separability.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 4: Scorecard `--fleet-curve` surface

**Files:**
- Modify: `tools/decoy_audit/scorecard.py` (add a helper + CLI flag)
- Test: `tools/decoy_audit/tests/test_scorecard.py` (add a `FleetCurve` class)

**Interfaces:**
- Consumes: `d_rssi_k` from `discriminators.py` (Task 3); the existing `scorecard.py` synth/profile loading and `import discriminators as D`.
- Produces: `rssi_fleet_curve(synth, profile, kmax)` returning `list[tuple[int, float]]` (`(k, separability)` for `k` in `1..kmax`); a `--fleet-curve KMAX` CLI flag that prints the curve.

- [ ] **Step 1: Write the failing test**

Add to `tools/decoy_audit/tests/test_scorecard.py` (new class at the end, before the `if __name__` guard):

```python
class FleetCurve(unittest.TestCase):
    def setUp(self):
        import capture_profile as cp
        self.synth = [{"atype": "static", "itvl_ms": 150, "company": 0x75,
                       "tx": t} for t in ([-12, -9, -6, -3, 0, 3] * 40)]
        broad = cp.rssi_hist(list(range(-95, -35)))
        self.profile = {"atype": {"static": 1.0, "rpa": 0.0, "public": 0.0},
                        "itvl_bins": [0, 0, 1.0, 0, 0, 0, 0], "vendor": {str(0x75): 1.0},
                        "rssi_bins": broad["rssi_bins"], "rssi_median": broad["rssi_median"]}

    def test_curve_length_and_k1(self):
        import discriminators as D
        curve = SC.rssi_fleet_curve(self.synth, self.profile, 5)
        self.assertEqual([k for k, _ in curve], [1, 2, 3, 4, 5])
        self.assertEqual(curve[0][1], round(D.d_rssi(self.synth, self.profile), 4))

    def test_curve_non_increasing(self):
        curve = SC.rssi_fleet_curve(self.synth, self.profile, 6)
        seps = [s for _, s in curve]
        for a, b in zip(seps, seps[1:]):
            self.assertLessEqual(b, a + 1e-9)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_scorecard.py -q -k FleetCurve`
Expected: FAIL — `AttributeError: module 'scorecard' has no attribute 'rssi_fleet_curve'`.

- [ ] **Step 3: Implement in `tools/decoy_audit/scorecard.py`**

Add the helper (near the other scoring helpers, alongside `build_scorecard`):

```python
def rssi_fleet_curve(synth, profile, kmax):
    """Modeled RSSI separability vs fleet size: [(k, sep), ...] for k in 1..kmax.
    Quantifies the mesh win -- k=1 is the single-node number; larger k spreads the crowd
    across k anchored points. Honest ceiling: k points, not one-per-device."""
    return [(k, round(D.d_rssi_k(synth, profile, k), 4)) for k in range(1, max(1, kmax) + 1)]
```

In the CLI arg parser (where `--gate`, `--devices`, `--atype-detail` are added), add:

```python
    ap.add_argument("--fleet-curve", type=int, default=0, metavar="KMAX",
                    help="print modeled RSSI separability for fleet sizes 1..KMAX and exit")
```

And handle it after the synth + profile are loaded (before the normal scorecard print), so it can run standalone:

```python
    if args.fleet_curve:
        curve = rssi_fleet_curve(synth, profile, args.fleet_curve)
        print("fleet-size  rssi_separability   (honest ceiling: K points, not one-per-device)")
        for k, sep in curve:
            print(f"  K={k:<3d}     {sep:.4f}")
        return 0
```

(Use the same `synth`/`profile` variable names the existing `main()` already builds. If `main()` is not structured to `return`, guard the normal path with `else:` or `sys.exit(0)` after printing — match the file's existing control flow.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd tools/decoy_audit && "C:/Program Files/Python312/python.exe" -m pytest tests/test_scorecard.py -q`
Expected: PASS (existing scorecard tests + 2 new). Then run the whole suite:
`"C:/Program Files/Python312/python.exe" -m pytest -q` → Expected: all pass.

- [ ] **Step 5: Capture the real curve for the docs**

Run the curve against the real capture and record the numbers for Task 5:

```powershell
"C:/Program Files/Python312/python.exe" scorecard.py ..\..\private\synth.ndjson ..\..\private\profile.json --fleet-curve 4
```

(If `private/synth.ndjson` / `profile.json` are absent, regenerate them first with `.\run.ps1`.) Note the K=1..4 separability values — they are the honest, measured mesh curve cited in Task 5.

- [ ] **Step 6: Commit**

```bash
git add tools/decoy_audit/scorecard.py tools/decoy_audit/tests/test_scorecard.py
git commit -F - <<'EOF'
feat(decoy_audit): scorecard --fleet-curve (RSSI separability vs K)

rssi_fleet_curve() surfaces the measured mesh win as a K -> separability table.
K=1 is the single-node number; the curve falls as nodes spread the crowd.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 5: Roadmap update with the measured curve

**Files:**
- Modify: `docs/ROADMAP.md` (the M10 bullet and the "One radio can't forge N…" honest-ceiling note)

**Interfaces:** none (docs only).

- [ ] **Step 1: Update the M10 bullet**

In `docs/ROADMAP.md`, the M10 bullet currently ends by describing mesh-distributed personas as "remain future." Change that clause to record v2 as landed and cite the mechanism + the measured curve from Task 4 Step 5. Use the exact numbers captured there; keep the corrected framing ("distributing whole personas across nodes"). Example (substitute the real measured values for `<...>`):

```markdown
  points) to beat the crowd-level co-location / RSSI tell — **mesh v2 landed 2026-07-17
  (`-DSIMULACRA_FLEET_SIZE=K`: each node runs 1/K of the population; whole personas distributed, not
  a persona's radios). The audit measures the win: modeled RSSI separability falls K=1 <...> ->
  K=2 <...> -> K=3 <...> (`scorecard.py --fleet-curve`).** Note: splitting a single persona's two
  radios across nodes would be wrong — it manufactures a "phone whose radios are metres apart" tell
  no real device has; the fix is spatial diversity of the *crowd*, not the pair.
```

- [ ] **Step 2: Update the honest-ceiling note**

In the "One radio can't forge N hardware fingerprints" bullet, append a sentence tying the RSSI tell to the mesh result and stating the ceiling verbatim:

```markdown
  Mesh v2 (`-DSIMULACRA_FLEET_SIZE=K`) now distributes the crowd across K nodes, which the audit
  shows lowers the modeled RSSI separability proportionally — **but the honest ceiling stands: K
  nodes give K points, not one-per-device; the win is proportional to node count, not a cure.**
```

- [ ] **Step 3: Verify no placeholders remain**

Re-read the two edited passages; confirm every `<...>` has been replaced with a real measured number and the honest-ceiling sentence is present. Run a quick PII scan on the file — scan for absolute user paths here, and separately confirm the file contains neither the OS username nor the forbidden committer name (the exact tokens live in the gitignored `private/PROJECT-MAP.md` §12; **never hardcode them into a tracked doc — that is itself the leak**):

```bash
grep -nE "C:\\\\Users\\\\|/Users/|/home/" docs/ROADMAP.md || echo "no absolute paths"
```

Expected: `no absolute paths`, and a manual check against the private forbidden-token list comes back clean.

- [ ] **Step 4: Commit**

```bash
git add docs/ROADMAP.md
git commit -F - <<'EOF'
docs(roadmap): record mesh v2 landed + the measured RSSI-vs-K curve

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

## Finishing

After all tasks pass and the full audit suite is green (`"C:/Program Files/Python312/python.exe" -m pytest -q` from `tools/decoy_audit/`), use **superpowers:finishing-a-development-branch**:
- Verify tests, then merge `feat/mesh-distributed-personas` to `main` locally `--no-ff`.
- PII-scan the merged diff range (`C:\Users`, `/Users/`, `/home/`, the OS username, real MACs) before pushing.
- Push `main` (the user keeps the live version correct).
- Update `private/PROJECT-MAP.md` §11 (mesh personas: DONE, with the measured curve) and the memory notes per the self-learning directive.

## Self-Review

**Spec coverage:** (1) `fleet_pop` module w/ default-1 identity → Task 1. (2) CMake forwarder + firmware build → Task 2. (3) chokepoint scaling incl. fallback + floor + persona count → Task 2. (4) K-node anchored RSSI model, K=1 exact, monotonic → Task 3. (5) scorecard/CLI surface → Task 4. Honest ceiling into docs → Task 5. Threat-model "no radio-splitting / i.i.d. composition" is preserved (Task 2 scales counts only; no per-node identity introduced). All spec sections map to a task.

**Placeholder scan:** the only intentional `<...>` are the measured curve numbers in Task 5, which Task 4 Step 5 produces and Task 5 Step 3 verifies are replaced — not a plan gap.

**Type consistency:** `fleet_pop_share`/`fleet_pop_share_k`/`fleet_pop_size` names identical across Tasks 1–2; `d_rssi_k(synth, profile, k)` and `_sample_bases_from_bins(rssi_bins, k, rng)` identical across Tasks 3–4; `rssi_fleet_curve(synth, profile, kmax)` returns `(k, sep)` tuples consumed consistently in Task 4's test.
