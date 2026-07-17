# Persona-Active Address-Type Measurement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure the persona-induced BLE address-type tilt for real (instead of the analytic ~79%-RPA estimate) by feeding the persona-active live BLE population through the existing `address_type_mix` discriminator, and surface it interpretably in the scorecard.

**Architecture:** A new `synth_dump --persona-pop` mode runs the full persona system and emits the live `ble_device` population (bound RPA personas + unbound static/NRPA/persistent crowd) as NDJSON in the exact roster-synth shape, so the existing `scorecard.py`/`discriminators.py` score it unchanged. A `scorecard --atype-detail` flag prints the raw decoy-vs-real `static/rpa/public` fractions so the separability number is interpretable; `run.ps1` adds a second "persona-active" scorecard line.

**Tech Stack:** C (host-compiled with MSVC `cl` against `tools/decoy_audit/host_stubs/`); Python `unittest` under pytest.

## Global Constraints

- **Build path (Windows):** the decoy_audit host tool is built by MSVC `cl` from `tools/decoy_audit/run.ps1` (the `cl` line is the source of truth; the `Makefile` is Unix parity). No new source FILES are added — `--persona-pop` lives inside the already-compiled `synth_dump.c`, so **no build-source-list change is needed**. Build the exe with the explicit `cl` command shown in the steps (its `run.ps1 -Rebuild` runs a full audit pipeline needing a capture — use the direct `cl` for a plain build). `cl` is on PATH in a Developer PowerShell for VS.
- **Tests run with:** `"C:/Program Files/Python312/python.exe" -m pytest <path> -v`.
- **NDJSON row shape (must match the roster synth exactly, so the scorecard consumes it unchanged):**
  `{"addr":"<hex12>","atype":"<static|rpa|public>","company":<u>,"itvl_ms":<u>,"tx":<d>,"arch":<u>,"plen":<u>,"ad":"<csv>"}`
- **Reference:** measure against the existing `long.pcap`-derived `profile.json`; also surface raw decoy/real atype fractions. (Do NOT add a new capture — out of scope.)
- **Branch:** create `feat/persona-atype-measurement` off `main`. Do NOT push. Merge locally with `--no-ff` at the end (finishing-a-development-branch).
- **Commit trailer (every commit), append exactly:**
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
  ```

---

### Task 1: `synth_dump --persona-pop` — dump the persona-active live BLE population

**Files:**
- Modify: `tools/decoy_audit/synth_dump.c` (add a `--persona-pop` mode in `main`, near the existing `--personas` block at line ~109)
- Test: `tools/decoy_audit/tests/test_persona_atype.py` (new)

**Interfaces:**
- Consumes (all already compiled into the decoy_audit build): `roster_init`, `phantom_init`, `ble_devices_init`, `probe_agents_init`, `phantom_lifecycle`, `phantom_sync_wifi`, `phantom_sync_ble`, `ble_devices_tick`, `ble_devices_count`, `ble_devices_at`, and the file-local helpers `atype_of`, `company_onair`, `ad_types_onair`.
- Produces: `synth_dump --persona-pop <seed> <nph> <ndev> <ticks> <tick_ms>` → NDJSON, one row per live `ble_device`, in the roster-synth shape.

- [ ] **Step 1: Create the branch**

```bash
git switch -c feat/persona-atype-measurement
```

- [ ] **Step 2: Write the failing test**

Create `tools/decoy_audit/tests/test_persona_atype.py`:

```python
import json, os, subprocess, unittest
from collections import Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def persona_pop(seed, nph=16, ndev=24, ticks=4000, tick_ms=1000):
    out = subprocess.check_output(
        [EXE, "--persona-pop", str(seed), str(nph), str(ndev), str(ticks), str(tick_ms)], text=True)
    return [json.loads(l) for l in out.splitlines() if l.strip()]


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class PersonaAtype(unittest.TestCase):
    def test_pop_has_both_rpa_and_non_rpa(self):
        rows = persona_pop(1)
        self.assertGreaterEqual(len(rows), 24, "persona-pop did not emit the full population")
        atypes = Counter(r["atype"] for r in rows)
        self.assertIn("rpa", atypes, "no RPA rows (bound personas missing)")
        self.assertTrue(atypes.get("static", 0) or atypes.get("public", 0),
                        "no non-RPA rows (unbound static/NRPA crowd missing)")

    def test_rpa_fraction_elevated_by_personas(self):
        rows = persona_pop(1)
        rpa = sum(1 for r in rows if r["atype"] == "rpa") / len(rows)
        # personas force 16 bound RPA + 8 unbound (~52/36/12) => RPA well above the roster's ~0.36
        self.assertGreater(rpa, 0.6, f"RPA fraction {rpa:.2f} not persona-elevated")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 3: Build and run the test to verify it FAILS**

From a Developer PowerShell for VS:
```powershell
cd tools/decoy_audit
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c roster_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c /Fe:synth_dump.exe
"C:/Program Files/Python312/python.exe" -m pytest tests/test_persona_atype.py -v
```
Expected: both tests **FAIL** — `--persona-pop` is an unknown arg, so `synth_dump` falls through to its default roster-generation mode and prints roster NDJSON (which uses a model-seed it wasn't given) / no `--persona-pop` rows; the subprocess output won't parse as the expected population. (If it errors instead of asserting, that still counts as RED.)

- [ ] **Step 4: Implement the `--persona-pop` mode**

In `tools/decoy_audit/synth_dump.c`, add this block at the very start of `main` (immediately before the existing `if (argc > 1 && strcmp(argv[1], "--personas") == 0)` block, ~line 109):

```c
    if (argc > 1 && strcmp(argv[1], "--persona-pop") == 0) {
        unsigned seed   = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 1;
        int      nph    = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 16;
        int      ndev   = argc > 4 ? (int)strtoul(argv[4], 0, 10) : 24;
        int      ticks  = argc > 5 ? (int)strtoul(argv[5], 0, 10) : 4000;
        unsigned tickms = argc > 6 ? (unsigned)strtoul(argv[6], 0, 10) : 1000;
        srand(seed);
        roster_init();
        uint32_t t = 0;
        phantom_init(nph, t);
        ble_devices_init(ndev, t);
        probe_agents_init(nph, t);
        phantom_sync_wifi(t);
        phantom_sync_ble(t);
        for (int s = 0; s <= ticks; s++) {
            if (s) t += tickms;
            phantom_lifecycle(t);
            phantom_sync_wifi(t);
            phantom_sync_ble(t);
            ble_devices_tick(t);
        }
        // Snapshot the standing live BLE population (bound RPA personas + unbound crowd) as NDJSON
        // in the SAME shape the roster synth emits, so the existing scorecard scores it unchanged.
        for (int i = 0; i < ble_devices_count(); i++) {
            const ble_device_t *d = ble_devices_at(i);
            char hex[13];
            for (int b = 0; b < 6; b++) sprintf(hex + b*2, "%02x", d->id.addr[b]);
            char adsig[48]; ad_types_onair(d->id.payload, d->id.payload_len, adsig, sizeof adsig);
            printf("{\"addr\":\"%s\",\"atype\":\"%s\",\"company\":%u,\"itvl_ms\":%u,"
                   "\"tx\":%d,\"arch\":%u,\"plen\":%u,\"ad\":\"%s\"}\n",
                   hex, atype_of(d->id.addr), company_onair(d->id.payload, d->id.payload_len),
                   d->id.adv_itvl_ms, d->id.tx_power, d->id.archetype_idx, d->id.payload_len, adsig);
        }
        return 0;
    }
```

- [ ] **Step 5: Rebuild and run the test to verify it PASSES**

```powershell
cd tools/decoy_audit
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar synth_dump.c ble_hs_adv.c roster_stub.c ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c /Fe:synth_dump.exe
"C:/Program Files/Python312/python.exe" -m pytest tests/test_persona_atype.py -v
```
Expected: both tests PASS. Sanity-eyeball once: `.\synth_dump.exe --persona-pop 1 16 24 200 1000 | Select-Object -First 2` should print two JSON rows, at least one `"atype":"rpa"`.

- [ ] **Step 6: Confirm the existing suite still passes**

```powershell
"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests -q
```
Expected: all pass (the new file adds 2; nothing else changed).

- [ ] **Step 7: Commit**

```bash
git add tools/decoy_audit/synth_dump.c tools/decoy_audit/tests/test_persona_atype.py
git commit -m "feat(audit): synth_dump --persona-pop dumps the persona-active BLE population"
```

---

### Task 2: `scorecard --atype-detail` + `run.ps1` persona-active line

**Files:**
- Modify: `tools/decoy_audit/scorecard.py` (add the `--atype-detail` flag + print block)
- Modify: `tools/decoy_audit/run.ps1` (add a persona-pop generation + a second scorecard line)
- Test: `tools/decoy_audit/tests/test_persona_atype.py` (add one test)

**Interfaces:**
- Consumes: `synth_dump --persona-pop` (Task 1); `discriminators.synth_distributions(synth)["atype"]` (returns `[static, rpa, public]`); the scorecard's already-loaded `profile["atype"]` dict.
- Produces: `scorecard.py <synth> <profile> --atype-detail` prints one extra line: `atype detail   decoy static/rpa/public = a/b/c   real = x/y/z`.

- [ ] **Step 1: Write the failing test**

Add to `tools/decoy_audit/tests/test_persona_atype.py` (inside the `PersonaAtype` class):

```python
    def test_atype_detail_prints_decoy_and_real_fractions(self):
        import tempfile
        rows = persona_pop(1)
        with tempfile.TemporaryDirectory() as td:
            synth = os.path.join(td, "pop.ndjson")
            with open(synth, "w") as f:
                for r in rows: f.write(json.dumps(r) + "\n")
            prof = os.path.join(td, "profile.json")
            with open(prof, "w") as f:
                json.dump({"atype": {"static": 0.52, "rpa": 0.36, "public": 0.12},
                           "itvl_bins": [0, 0, 1, 0, 0, 0, 0], "vendor": {"none": 1.0}}, f)
            out = subprocess.check_output(
                ["C:/Program Files/Python312/python.exe" if os.name == "nt" else "python3",
                 os.path.join(TOOL, "scorecard.py"), synth, prof, "--atype-detail"], text=True)
        self.assertIn("atype detail", out)
        self.assertIn("decoy", out); self.assertIn("real", out)
```

- [ ] **Step 2: Run the test to verify it FAILS**

```powershell
"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests/test_persona_atype.py::PersonaAtype::test_atype_detail_prints_decoy_and_real_fractions -v
```
Expected: FAIL — `--atype-detail` is an unrecognized argument (argparse exits 2), so `check_output` raises `CalledProcessError`.

- [ ] **Step 3: Implement the `--atype-detail` flag**

In `tools/decoy_audit/scorecard.py`, add the argument (after the `--gate` line in `main`):

```python
    ap.add_argument("--atype-detail", action="store_true",
                    help="print decoy vs real static/rpa/public fractions under the scorecard")
```

And add this block right after the `for d in card["discriminators"]: print(...)` loop (before the `HEADLINE` print):

```python
    if a.atype_detail:
        dec = D.synth_distributions(synth)["atype"]                       # [static, rpa, public]
        real = [profile["atype"].get(k, 0) for k in ("static", "rpa", "public")]
        print("atype detail   decoy static/rpa/public = %.2f/%.2f/%.2f   real = %.2f/%.2f/%.2f"
              % (dec[0], dec[1], dec[2], real[0], real[1], real[2]))
```

- [ ] **Step 4: Run the test to verify it PASSES**

```powershell
"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests/test_persona_atype.py -v
```
Expected: all 3 tests PASS.

- [ ] **Step 5: Wire the persona-active line into `run.ps1`**

In `tools/decoy_audit/run.ps1`, find the final scorecard invocation (the block that builds `$scArgs` and runs `python @scArgs`, near the end). Immediately AFTER that block, add:

```powershell
# --- persona-active address-type reality (M10 personas tilt the BLE mix toward RPA) ---
Write-Host "[persona] address-type mix with personas active ..." -ForegroundColor Cyan
$ppop = & $exe --persona-pop $Seed 16 24 4000 1000
if ($LASTEXITCODE -eq 0) {
    $ppopFile = Join-Path $OutDir "persona_pop.ndjson"
    Set-Content -Path $ppopFile -Value $ppop -Encoding ascii
    python (Join-Path $tool "scorecard.py") $ppopFile $profileJson "--atype-detail"
} else {
    Write-Host "persona-pop generation failed (rc=$LASTEXITCODE) -> skipped" -ForegroundColor DarkYellow
}
```

(`$exe`, `$Seed`, `$OutDir`, `$profileJson`, `$tool` are all already defined earlier in `run.ps1`.)

- [ ] **Step 6: Smoke-test run.ps1's new step in isolation**

The full `run.ps1` needs `private/long.pcap`; if present, run `.\tools\decoy_audit\run.ps1` and confirm a `[persona]` section prints a scorecard with an `atype detail` line. If `long.pcap` is absent, verify the two commands directly instead:
```powershell
cd tools/decoy_audit
$rows = .\synth_dump.exe --persona-pop 1 16 24 4000 1000
$rows | Measure-Object   # should report Count >= 24
```
Expected: a non-empty population dump (the scorecard half is already covered by the Step-4 test).

- [ ] **Step 7: Full suite + commit**

```powershell
"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests -q
```
Expected: all pass.

```bash
git add tools/decoy_audit/scorecard.py tools/decoy_audit/run.ps1 tools/decoy_audit/tests/test_persona_atype.py
git commit -m "feat(audit): scorecard --atype-detail + run.ps1 persona-active atype line"
```

- [ ] **Step 8: Report the measured number**

Run the persona-active scorecard against the real profile and record the actual `address_type_mix` separability + the raw fractions in the commit or a short note, so the follow-up decision (accept / re-tune density / phone-dense reference) has data:
```powershell
cd tools/decoy_audit
.\synth_dump.exe --persona-pop 1 16 24 4000 1000 | Set-Content -Encoding ascii ..\..\private\persona_pop.ndjson
python scorecard.py ..\..\private\persona_pop.ndjson ..\..\private\profile.json --atype-detail
```
(Requires `private/profile.json` from a prior `run.ps1`. If absent, generate it first with `python capture_profile.py ..\..\private\long.pcap ..\..\private\profile.json ..\..\private\model.seed`.) Capture the printed `address_type_mix` line + `atype detail` line into the finish notes.

---

### Finish

Invoke **superpowers:finishing-a-development-branch**. Target: merge `feat/persona-atype-measurement` → `main` locally with `--no-ff`; do NOT push. Verify `pytest tools/decoy_audit/tests` is green before merging. Include the measured `address_type_mix` number (Task 2 Step 8) in the merge notes.

## Notes for the implementer

- `--persona-pop` reuses the exact roster-synth NDJSON row format so the scorecard/discriminators need **no changes** to score it — that is the whole point; do not add a persona-specific discriminator.
- The dump is a **snapshot of the standing live population** after the tick loop (not per-event) — that is the atype composition an observer sees at any instant, which is what the `address_type_mix` tell measures.
- `atype_of` maps NRPA (top-2 bits `00`) to `"public"`, exactly as `capture_profile._atype` does on the real side, so the two histograms compare like-for-like. Do not "fix" this to a separate `nrpa` bucket — it would desynchronize the comparison.
- This measures vs `long.pcap` (static-heavy) on purpose; a high number is expected and the `--atype-detail` fractions make the reason visible. Do not change the reference here.
