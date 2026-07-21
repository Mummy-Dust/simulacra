# Persona RPA Address-Type Rebalance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retune two firmware constants so persona RPAs become a minority of the BLE crowd instead of the majority, measurably closing the `address_type_mix` residual.

**Architecture:** Two coupled constant changes in `main/ble_devices.c` and `main/probe.c` (one coherent composition rebalance — not independently meaningful split apart), verified by measuring `decoy_audit`'s existing `--persona-pop` + `scorecard.py --atype-detail` pipeline before and after, plus a full regression run of both host suites.

**Tech Stack:** C (ESP-IDF firmware; MSVC `cl` for the host harness). No new test infrastructure — verification reuses `tools/decoy_audit`'s existing measurement tools.

## Global Constraints

- `main/ble_devices.c`: `ATYPE_STATIC_W`/`ATYPE_RPA_W`/`ATYPE_NRPA_W` **52/36/12 → 75/5/20**.
- `main/probe.c`: `PHANTOM_BLE_UNBOUND` **8 → 16** (shared constant, both `esp32c5`/`esp32c6`).
- Projected (expected-value, not guaranteed): `address_type_mix` ≈0.070 → ≈0.019 on C5 (`nph=16, ndev=32`).
- Verification is **measurement-based** (rerun the existing audit pipeline before/after) — no new pytest assertions are required for the projection itself; existing tests must still pass.
- Root `sdkconfig` is currently target **esp32c5** — compile-verify with `build_c5` (no `set-target`, avoids a slow reconfigure).
- **Never hardcode the OS username / forbidden committer name into a tracked file** (`private/TOOLING-GOTCHAS.md`).
- Commit identity `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>`; every commit ends with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.
- Branch `feat/persona-atype-rebalance`; merge to `main` `--no-ff`, PII-scan, push at the end.

## Pre-flight check (already done during planning — recorded here for the record)

Grepped `tools/decoy_audit/tests/` for hardcoded old-weight/pool-size assumptions:
- `test_synth_dump.py::test_address_type_is_a_realistic_mix` uses the **plain** `synth_dump <seed> <n>` mode, which is driven by `roster.c`'s own independent (unchanged) 52/36/12 mix, **not** `ble_devices.c`'s constants. **Not affected.**
- `test_persona_atype.py::test_rpa_fraction_elevated_by_personas` calls `persona_pop(1)` with defaults `nph=16, ndev=24` and asserts `rpa > 0.6`. This directly encodes the **old** unbound pool size (24 = 16 + old 8) in its default and has a stale comment ("8 unbound (~52/36/12)"). The `ndev` default is a **test-harness parameter**, independent of the firmware `PHANTOM_BLE_UNBOUND` constant — it does not need to change for the test to remain valid, but the comment is now inaccurate and must be fixed (Task 1 Step 8). The threshold (`rpa > 0.6`) still holds after the weight change at this pool ratio (16 bound RPA + ~5% of 8 unbound ≈ 16.4/24 ≈ 0.68 > 0.6) — verified by actually running the test in Step 9, not by this arithmetic alone.

---

## File Structure

- **Modify** `main/ble_devices.c` — the atype weight constants (lines 9-11).
- **Modify** `main/probe.c` — `PHANTOM_BLE_UNBOUND` (line 44).
- **Modify** `tools/decoy_audit/tests/test_persona_atype.py` — fix the stale comment (line 27); no logic change.

---

### Task 1: Rebalance the unbound crowd composition, measure before/after

**Files:**
- Modify: `main/ble_devices.c:9-11`
- Modify: `main/probe.c:44`
- Modify: `tools/decoy_audit/tests/test_persona_atype.py:27`

**Interfaces:** none (constant tuning only; no new symbols).

- [ ] **Step 1: Rebuild `synth_dump.exe` on the CURRENT (unmodified) code**

From a Developer PowerShell for VS, in `tools/decoy_audit`:

```powershell
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c roster_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c ..\..\main\fleet_pop.c /Fe:synth_dump.exe
```

Expected: compiles clean.

- [ ] **Step 2: Measure the BEFORE baseline (current real C5 composition: 16 personas + 8 unbound = 24)**

```powershell
.\synth_dump.exe --persona-pop 1 16 24 4000 1000 > ..\..\private\pp_before.ndjson
"C:/Program Files/Python312/python.exe" scorecard.py ..\..\private\pp_before.ndjson ..\..\private\profile.json --atype-detail
```

Expected output includes an `atype detail   decoy static/rpa/public = ...` line and an
`address_type_mix` score near the previously-measured **≈0.070** (confirms the harness reproduces
the known baseline before any code change). Record the exact printed decoy fractions and the score.

- [ ] **Step 3: Change the unbound atype weights in `main/ble_devices.c`**

Replace:

```c
// Address-subtype blend, matching roster.c's calibrated fleet mix (~52/36/12 static/RPA/NRPA).
#define ATYPE_STATIC_W  52
#define ATYPE_RPA_W     36
#define ATYPE_NRPA_W    12
```

with:

```c
// Address-subtype blend for the UNBOUND crowd (beacons/wearables/tags): almost never RPA in
// reality (RPA is a phone/OS behavior). Deliberately diverges from roster.c's phone-influenced
// 52/36/12 mix so persona RPAs (always RPA) don't dominate the aggregate BLE crowd (see
// docs/superpowers/specs/2026-07-21-persona-atype-rebalance-design.md).
#define ATYPE_STATIC_W  75
#define ATYPE_RPA_W      5
#define ATYPE_NRPA_W    20
```

- [ ] **Step 4: Grow the unbound pool in `main/probe.c`**

Replace:

```c
// Personas need one BLE slot each PLUS this many unbound BLE-only decoys so the static/NRPA/
// persistent mix survives (the address-type + presence tells we already closed).
#define PHANTOM_BLE_UNBOUND 8
```

with:

```c
// Personas need one BLE slot each PLUS this many unbound BLE-only decoys so the static/NRPA/
// persistent mix survives (the address-type + presence tells we already closed), and so the
// unbound (non-phone) crowd is large enough to keep persona RPAs a MINORITY of the aggregate
// (see docs/superpowers/specs/2026-07-21-persona-atype-rebalance-design.md). Shared C5/C6 constant.
#define PHANTOM_BLE_UNBOUND 16
```

- [ ] **Step 5: Rebuild `synth_dump.exe` with the changes**

Rerun the exact `cl` command from Step 1.

- [ ] **Step 6: Measure the AFTER result (new real C5 composition: 16 personas + 16 unbound = 32)**

```powershell
.\synth_dump.exe --persona-pop 1 16 32 4000 1000 > ..\..\private\pp_after.ndjson
"C:/Program Files/Python312/python.exe" scorecard.py ..\..\private\pp_after.ndjson ..\..\private\profile.json --atype-detail
```

Expected: `address_type_mix` has dropped substantially from the Step 2 baseline, in the direction
and rough magnitude of the ≈0.070 → ≈0.019 projection (an expected-value estimate — the exact
number will vary by seed; a large, clear drop is the pass criterion, not an exact match).

- [ ] **Step 7: Run the full `decoy_audit` host suite**

```powershell
"C:/Program Files/Python312/python.exe" -m pytest -q
```

Expected: all tests pass (was 97 before this change). If `test_persona_atype.py::test_rpa_fraction_elevated_by_personas` fails, recompute its actual measured value from the output and adjust the threshold in Step 8 to a value comfortably below the observed fraction and comfortably above real's ~0.36 baseline — do not loosen it further than needed to pass.

- [ ] **Step 8: Fix the stale comment in `tools/decoy_audit/tests/test_persona_atype.py`**

Replace:

```python
        # personas force 16 bound RPA + 8 unbound (~52/36/12) => RPA well above the roster's ~0.36
```

with:

```python
        # personas force 16 bound RPA + 8 unbound (non-phone-weighted, mostly static/NRPA since
        # 2026-07-21) => RPA well above the roster's ~0.36. This test uses its own smaller ndev=24
        # (not the live PHANTOM_BLE_UNBOUND=16) purely as a persona-elevation regression check.
```

(No logic change — this test intentionally uses a fixed `ndev=24` harness parameter, independent of
the live firmware pool size, as a stable regression check that personas measurably elevate RPA share
above baseline. See the Pre-flight check above.)

- [ ] **Step 9: Re-run the full `decoy_audit` suite to confirm the comment fix didn't break anything**

```powershell
"C:/Program Files/Python312/python.exe" -m pytest -q
```

Expected: all pass (97).

- [ ] **Step 10: Run the full `probe_audit` host suite (cross-check — `PHANTOM_BLE_UNBOUND` feeds `probe_desired_ble_floor()`)**

```powershell
cd ..\probe_audit
"C:/Program Files/Python312/python.exe" -m pytest -q
```

Expected: all pass (44) — `probe_desired_ble_floor()` (`main/probe.c`) only affects the BLE population
floor computed in `simulacra_main.c` (not host-compiled/tested), so this suite is a sanity check that
nothing in the Wi-Fi probe harness broke, not a direct exercise of the changed constant.

- [ ] **Step 11: Compile-verify firmware (esp32c5, matches the current root sdkconfig)**

Activate ESP-IDF, then from the repo root:

```powershell
idf.py -B build_c5 build
```

Expected: build succeeds (both changed files recompile cleanly into the firmware image).

- [ ] **Step 12: Commit**

```bash
cd ../..
git add main/ble_devices.c main/probe.c tools/decoy_audit/tests/test_persona_atype.py
git commit -F - <<'EOF'
feat(persona-atype): rebalance BLE crowd composition (RPA minority)

Retune the unbound crowd's atype weights (52/36/12 -> 75/5/20 static/RPA/NRPA
-- beacons/wearables/tags are almost never RPA) and grow PHANTOM_BLE_UNBOUND
(8->16, shared C5/C6) so persona RPAs (always RPA) become a minority of the
aggregate BLE crowd instead of the majority (C5: 67%->50%, C6: 50%->33%).

Measured: address_type_mix <BEFORE> -> <AFTER> (persona-pop nph=16 ndev=24->32
vs private/profile.json). Compile-verified esp32c5; decoy_audit 97 / probe_audit
44 unaffected (test_persona_atype.py comment updated to describe the new
weights; its ndev=24/threshold are an independent regression check, unchanged).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

Before committing, replace `<BEFORE>` and `<AFTER>` in the commit message with the actual numbers
measured in Steps 2 and 6.

---

## Finishing

Use **superpowers:finishing-a-development-branch**:
- Both host suites already confirmed green (Steps 7/9/10); firmware compile-verified (Step 11).
- Merge `feat/persona-atype-rebalance` to `main` `--no-ff`; PII-scan the range (`C:\Users`, `/Users/`,
  `/home/`, the OS username, real MACs); push `main`.
- Update `private/PROJECT-MAP.md` §11 and memory `cross-protocol-personas.md` with the measured
  before/after `address_type_mix` numbers (from Steps 2 and 6) and mark the persona atype-tilt
  follow-up closed.

## Self-Review

**Spec coverage:** weight retune (75/5/20) → Task 1 Step 3; `PHANTOM_BLE_UNBOUND` 8→16 → Step 4; C5/C6
table impact → Global Constraints (implicit in the shared-constant change, both targets recompile
identically — no per-target code needed); hardware-cost claims (no new RAM/radio) → already verified
during design, not re-verified here since they follow from `CHURN_HW_INSTANCES`/`BLE_DEVICES_MAX`
being unchanged; measurement-based verification (not new unit tests) → Steps 2/6; regression safety →
Steps 7/9/10 + the pre-flight test audit. All spec sections map to a task step.

**Placeholder scan:** `<BEFORE>`/`<AFTER>` in the Step 12 commit message are explicitly flagged as
values to fill in from the just-run measurements, not unresolved plan gaps — every other step has
concrete, runnable content.

**Type consistency:** N/A (constant-only change, no new interfaces). The `--persona-pop nph ndev`
argument order is used consistently in Steps 2 and 6 (`nph=16` fixed, `ndev` 24→32 matching the real
firmware composition before/after).
