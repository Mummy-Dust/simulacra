# Post-flash `en_sys_seq` Regression Gate — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A two-board bench gate that verifies an ESP32-C5 still honors `esp_wifi_80211_tx(..., en_sys_seq=false)` (per-source-MAC independent 802.11 sequence counters) after a flash or IDF bump, emitting a PASS/FAIL verdict.

**Architecture:** One board injects probe requests pinned to a single channel; a second board sniffs that channel and logs source-MAC + sequence number per frame; a host analyzer flags the shared-hardware-counter signature (a run of ≥3 time-ordered frames whose seq increments by +1 while the MAC changes). All firmware hooks are default-off build flags, so the shipped decoy is unaffected.

**Tech Stack:** ESP-IDF 5.5 (esp32c5), C, Python 3 (analyzer + pytest), PowerShell (runner).

## Global Constraints

- Public repo: NEVER commit absolute local paths, the OS username `user`, real hardware MACs, or real SSIDs. The runner assumes `idf.py` is already on PATH; it must not hardcode IDF install paths.
- Commit trailers required on every commit:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`
- Work on the current branch; do NOT `git push`.
- All new firmware flags default OFF (or to the current shipped default) — zero shipped-behavior change, no wire-struct change, no density cost.
- Target chip for all builds/validation: esp32c5 (ESP-IDF 5.5).
- Analyzer exit codes are a contract: `0`=PASS, `1`=FAIL, `2`=no frames parsed.

---

### Task 1: Firmware gate flags (injector + sniffer channel)

Add default-off build flags so a board can be built as a single-channel injector (optionally simulating the regression) and the sniffer's parked channel is controllable from the same `-Channel` argument.

**Files:**
- Modify: `main/probe.c` (add two flag blocks; use them in `probe_inject_burst` and `probe_task`)
- Modify: `main/sniff.c:16` (make `SNIFF_FIXED_CH` a `#ifndef` default)
- Modify: `main/CMakeLists.txt:12-17` (forward the three new flags)

**Interfaces:**
- Produces (build-time flags, all default-off / current-default):
  - `PROBE_FIX_CH` — int, default `0`. `>0` pins injection to that channel; `0` = normal hop.
  - `PROBE_FORCE_SHARED` — 0/1, default `0`. `1` = `esp_wifi_80211_tx(..., en_sys_seq=true)` (regression sim).
  - `SNIFF_FIXED_CH` — int, default `1`. Channel the sniffer parks on.

- [ ] **Step 1: Add the injector flag blocks to `main/probe.c`**

In `main/probe.c`, after the `#define PROBE_TX_FAIL_THRESH 16` block (currently ending near line 27), add:

```c
// --- en_sys_seq bench-gate hooks (default-off; see tools/seq_gate). Shipped decoy UNAFFECTED. ---
// The gate needs single-channel injection so the sniffer hears EVERY frame (a shared HW counter
// ticks on every TX regardless of channel; a hopping injector viewed on one channel hides the
// +1 signature). PROBE_FORCE_SHARED deliberately triggers the regression to prove the gate catches it.
#ifndef PROBE_FIX_CH
#define PROBE_FIX_CH 0        // >0: pin injection to this channel; 0 = normal 1/6/11(+5G) hop
#endif
#ifndef PROBE_FORCE_SHARED
#define PROBE_FORCE_SHARED 0  // 1: en_sys_seq=true (shared HW counter) — regression simulation only
#endif
```

- [ ] **Step 2: Use `PROBE_FORCE_SHARED` at the TX call in `probe_inject_burst`**

In `main/probe.c`, replace the existing TX line:

```c
        rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, false);  // en_sys_seq=false: per-agent seq
```

with:

```c
        // en_sys_seq=false honors the per-agent seq we wrote (the whole defense). The gate can force
        // true (PROBE_FORCE_SHARED) to simulate the shared-HW-counter regression and prove it's caught.
        rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, PROBE_FORCE_SHARED);  // 0 -> false (shipped)
```

- [ ] **Step 3: Use `PROBE_FIX_CH` in `probe_task`**

In `main/probe.c`, replace the body of `probe_task`'s loop:

```c
static void probe_task(void *arg)
{
    (void)arg;
    for (;;) {
        probe_inject_burst(next_channel());
        vTaskDelay(pdMS_TO_TICKS(PROBE_BURST_MS));
    }
}
```

with:

```c
static void probe_task(void *arg)
{
    (void)arg;
    for (;;) {
#if PROBE_FIX_CH
        probe_inject_burst(PROBE_FIX_CH);   // gate mode: single channel so the sniffer hears every frame
#else
        probe_inject_burst(next_channel());
#endif
        vTaskDelay(pdMS_TO_TICKS(PROBE_BURST_MS));
    }
}
```

- [ ] **Step 4: Make `SNIFF_FIXED_CH` forwardable in `main/sniff.c`**

In `main/sniff.c`, replace (currently line 16):

```c
#define SNIFF_FIXED_CH 1
```

with:

```c
#ifndef SNIFF_FIXED_CH
#define SNIFF_FIXED_CH 1
#endif
```

- [ ] **Step 5: Forward the flags in `main/CMakeLists.txt`**

In `main/CMakeLists.txt`, the `foreach(flag ...)` list currently ends with:

```cmake
    SIMULACRA_VBAT_ADC SIMULACRA_VBAT_ADC_GPIO SIMULACRA_VBAT_ADC_DIV SIMULACRA_VBAT_LOW_MV)
```

Change that final line to add the three gate flags:

```cmake
    SIMULACRA_VBAT_ADC SIMULACRA_VBAT_ADC_GPIO SIMULACRA_VBAT_ADC_DIV SIMULACRA_VBAT_LOW_MV
    PROBE_FIX_CH PROBE_FORCE_SHARED SNIFF_FIXED_CH)
```

- [ ] **Step 6: Verify the SHIPPED default build is unchanged (flags off) — it compiles**

Activate the ESP-IDF 5.5 env for esp32c5, then from the repo root:

Run: `idf.py set-target esp32c5 && idf.py build`
Expected: `Project build complete.` (default decoy build; none of the new flags defined → identical behavior)

- [ ] **Step 7: Verify the GATE injector build compiles with the flags set**

Run (fresh clean to avoid a sticky prior config):
`rm -rf build sdkconfig; idf.py set-target esp32c5; idf.py -DSIMULACRA_PROBE=1 -DPROBE_FIX_CH=1 -DPROBE_FORCE_SHARED=1 build`
Expected: `Project build complete.` (no undefined-symbol / preprocessor errors)

- [ ] **Step 8: Restore a clean tree for the next task**

Run: `rm -rf build sdkconfig`
(The build artifacts are gitignored, but removing them keeps later builds deterministic.)

- [ ] **Step 9: Commit**

```bash
git add main/probe.c main/sniff.c main/CMakeLists.txt
git commit -m "$(printf 'feat(seq-gate): default-off firmware flags for the en_sys_seq bench gate\n\nPROBE_FIX_CH pins injection to one channel; PROBE_FORCE_SHARED forces\nen_sys_seq=true to simulate the regression; SNIFF_FIXED_CH becomes\nforwardable so one -Channel drives both boards. All default-off /\ncurrent-default: shipped decoy unchanged.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy')"
```

---

### Task 2: Analyzer `--seq-only` mode + unit tests

The seq-independence detector already exists in `sniff_analyze.py`. Add a `--seq-only` flag so the gate runs *only* that check (the decorrelation/constellation checks are probe-realism metrics that would false-FAIL on a short bench capture), and lock the detector's behavior with unit tests.

**Files:**
- Modify: `tools/probe_audit/analyzers/sniff_analyze.py` (add `--seq-only`)
- Create: `tools/probe_audit/analyzers/tests/test_sniff_analyze.py`

**Interfaces:**
- Consumes: `sniff_analyze.check_seq_independence(frames) -> bool`, where `frames` is a list of `(t_ms:int, mac:str, seq:int, rssi:int)`. Returns `True` = independent (PASS), `False` = shared-counter signature found (FAIL). (Existing function, unchanged.)
- Produces: `sniff_analyze.py --seq-only <log>` runs only the seq check; exit `0`=PASS, `1`=FAIL, `2`=no frames.

- [ ] **Step 1: Write the failing unit tests**

Create `tools/probe_audit/analyzers/tests/test_sniff_analyze.py`:

```python
import os
import sys

# import the analyzer module (sibling package dir), without running main()
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import sniff_analyze as sa  # noqa: E402


def test_seq_independence_pass_independent_counters():
    # Each MAC carries its OWN counter; no cross-MAC run of +1 steps. -> PASS (True)
    frames = [
        (0, "02:00:00:00:00:01", 100, -50),
        (1, "02:00:00:00:00:02", 500, -50),
        (2, "02:00:00:00:00:01", 101, -50),
        (3, "02:00:00:00:00:02", 501, -50),
        (4, "02:00:00:00:00:03", 900, -50),
    ]
    assert sa.check_seq_independence(frames) is True


def test_seq_independence_fail_shared_counter():
    # One shared HW counter: seq +1 on every frame while the MAC changes. -> FAIL (False)
    frames = [
        (0, "02:00:00:00:00:01", 10, -50),
        (1, "02:00:00:00:00:02", 11, -50),
        (2, "02:00:00:00:00:03", 12, -50),
        (3, "02:00:00:00:00:04", 13, -50),
    ]
    assert sa.check_seq_independence(frames) is False


def test_seq_independence_wraps_12bit():
    # 802.11 seq is 12-bit; a shared counter wrapping 4095->0 across MACs must still FAIL.
    frames = [
        (0, "02:00:00:00:00:01", 4094, -50),
        (1, "02:00:00:00:00:02", 4095, -50),
        (2, "02:00:00:00:00:03", 0, -50),
        (3, "02:00:00:00:00:04", 1, -50),
    ]
    assert sa.check_seq_independence(frames) is False
```

- [ ] **Step 2: Run the tests to verify they pass against the EXISTING detector**

Run: `python -m pytest tools/probe_audit/analyzers/tests/test_sniff_analyze.py -v`
Expected: 3 passed. (These lock in the already-correct `check_seq_independence` before we touch `main()`. If any FAIL, stop — the detector regressed and the gate can't be trusted.)

- [ ] **Step 3: Add the `--seq-only` flag to `main()`**

In `tools/probe_audit/analyzers/sniff_analyze.py`, in `main()`, add the argument after the existing `--rssi-max` line:

```python
    ap.add_argument("--seq-only", action="store_true",
                    help="run ONLY the seq-independence check (the en_sys_seq regression gate)")
```

Then replace the existing verdict block:

```python
    ok = all([
        check_seq_independence(frames),
        check_decorrelation(frames),
        check_constellation(frames),
    ])
    print("\nRESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
```

with:

```python
    if a.seq_only:
        ok = check_seq_independence(frames)   # en_sys_seq regression gate: this check alone
    else:
        ok = all([
            check_seq_independence(frames),
            check_decorrelation(frames),
            check_constellation(frames),
        ])
    print("\nRESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
```

- [ ] **Step 4: Verify `--seq-only` end-to-end on synthetic logs (exit codes)**

Create two throwaway logs and check the exit codes:

Run:
```bash
printf 'W (100) sniff: pr sa=02:00:00:00:00:01 seq=100 rssi=-50\nW (110) sniff: pr sa=02:00:00:00:00:02 seq=500 rssi=-50\nW (120) sniff: pr sa=02:00:00:00:00:01 seq=101 rssi=-50\nW (130) sniff: pr sa=02:00:00:00:00:03 seq=900 rssi=-50\n' > /tmp/seq_pass.log
python tools/probe_audit/analyzers/sniff_analyze.py --seq-only /tmp/seq_pass.log; echo "exit=$?"
```
Expected: prints `RESULT: PASS`, `exit=0`.

Run:
```bash
printf 'W (100) sniff: pr sa=02:00:00:00:00:01 seq=10 rssi=-50\nW (110) sniff: pr sa=02:00:00:00:00:02 seq=11 rssi=-50\nW (120) sniff: pr sa=02:00:00:00:00:03 seq=12 rssi=-50\nW (130) sniff: pr sa=02:00:00:00:00:04 seq=13 rssi=-50\n' > /tmp/seq_fail.log
python tools/probe_audit/analyzers/sniff_analyze.py --seq-only /tmp/seq_fail.log; echo "exit=$?"
```
Expected: prints `RESULT: FAIL`, `exit=1`.

- [ ] **Step 5: Commit**

```bash
git add tools/probe_audit/analyzers/sniff_analyze.py tools/probe_audit/analyzers/tests/test_sniff_analyze.py
git commit -m "$(printf 'feat(seq-gate): --seq-only mode + unit tests for the seq-independence check\n\nRuns just the en_sys_seq regression detector (skips the probe-realism\nchecks that false-FAIL on a short bench capture). Tests lock PASS,\nshared-counter FAIL, and 12-bit-wrap FAIL.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy')"
```

---

### Task 3: Runner script + README

Orchestrate the gate: flash injector, flash sniffer, capture the sniffer's serial, run the analyzer, print a verdict. Self-contained and PII-free.

**Files:**
- Create: `tools/seq_gate/run_seq_gate.ps1`
- Create: `tools/seq_gate/README.md`

**Interfaces:**
- Consumes: firmware flags from Task 1 (`SIMULACRA_PROBE`, `PROBE_FIX_CH`, `PROBE_FORCE_SHARED`, `SIMULACRA_SNIFF`, `SNIFF_FIXED_CH`); analyzer `--seq-only` from Task 2.
- Produces: `run_seq_gate.ps1 -InjPort <COM> -SniffPort <COM> [-Shared] [-Channel <n>] [-CaptureSeconds <n>]` → prints `SEQ GATE: PASS|FAIL`, exits with the analyzer's code.

- [ ] **Step 1: Write the runner `tools/seq_gate/run_seq_gate.ps1`**

Create `tools/seq_gate/run_seq_gate.ps1`:

```powershell
<#
run_seq_gate.ps1 — post-flash en_sys_seq regression gate.

Verifies this IDF/chip still honors esp_wifi_80211_tx(..., en_sys_seq=false): one C5 injects
probe requests pinned to a single channel; a second C5 sniffs that channel; the analyzer flags the
shared-hardware-counter signature. See docs/superpowers/specs/2026-07-15-en-sys-seq-gate-design.md.

PREREQUISITE: activate your ESP-IDF 5.5 environment first (so `idf.py` is on PATH). Run from anywhere.

  # normal run -> expect PASS:
  .\tools\seq_gate\run_seq_gate.ps1 -InjPort COM12 -SniffPort COM16
  # simulated regression -> expect FAIL (proves the gate discriminates):
  .\tools\seq_gate\run_seq_gate.ps1 -InjPort COM12 -SniffPort COM16 -Shared

NOTE: both boards are left in PROBE/SNIFF mode. Reflash them with your normal decoy build afterward.
#>
param(
  [Parameter(Mandatory)][string]$InjPort,
  [Parameter(Mandatory)][string]$SniffPort,
  [switch]$Shared,
  [int]$Channel = 1,
  [int]$CaptureSeconds = 30
)

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
  Write-Output "ERR: idf.py not on PATH — activate your ESP-IDF 5.5 env first (export.ps1)."
  exit 3
}
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$analyzer = Join-Path $repo "tools\probe_audit\analyzers\sniff_analyze.py"
$log = Join-Path $env:TEMP "seq_gate_$SniffPort.log"
Set-Location $repo

function Clean-Build { Remove-Item "$repo\build","$repo\sdkconfig" -Recurse -Force -ErrorAction SilentlyContinue }

# --- 1) injector: single-channel PROBE, optionally forcing the shared-counter regression ---
Write-Output "[1/4] building + flashing injector ($InjPort, ch$Channel, shared=$($Shared.IsPresent))..."
Clean-Build
idf.py set-target esp32c5 *> "$env:TEMP\seq_gate_inj.log"
$injDefs = @("-DSIMULACRA_PROBE=1", "-DPROBE_FIX_CH=$Channel")
if ($Shared) { $injDefs += "-DPROBE_FORCE_SHARED=1" }
idf.py @injDefs build *>> "$env:TEMP\seq_gate_inj.log"
if ($LASTEXITCODE -ne 0) { Write-Output "ERR: injector build failed — see $env:TEMP\seq_gate_inj.log"; exit 3 }
idf.py @injDefs -p $InjPort flash *>> "$env:TEMP\seq_gate_inj.log"
if ($LASTEXITCODE -ne 0) { Write-Output "ERR: injector flash failed — see $env:TEMP\seq_gate_inj.log"; exit 3 }

# --- 2) sniffer: park on the SAME channel (clean build so the cached PROBE flag can't leak in) ---
Write-Output "[2/4] building + flashing sniffer ($SniffPort, ch$Channel)..."
Clean-Build
idf.py set-target esp32c5 *> "$env:TEMP\seq_gate_sniff.log"
$snfDefs = @("-DSIMULACRA_SNIFF=1", "-DSNIFF_FIXED_CH=$Channel")
idf.py @snfDefs build *>> "$env:TEMP\seq_gate_sniff.log"
if ($LASTEXITCODE -ne 0) { Write-Output "ERR: sniffer build failed — see $env:TEMP\seq_gate_sniff.log"; exit 3 }
idf.py @snfDefs -p $SniffPort flash *>> "$env:TEMP\seq_gate_sniff.log"
if ($LASTEXITCODE -ne 0) { Write-Output "ERR: sniffer flash failed — see $env:TEMP\seq_gate_sniff.log"; exit 3 }

# --- 3) capture the sniffer's serial (no reset: don't toggle DTR/RTS, just read the running board) ---
Write-Output "[3/4] capturing $SniffPort for ${CaptureSeconds}s..."
Start-Sleep -Seconds 2   # let the sniffer finish booting after the flash-reset
$sp = New-Object System.IO.Ports.SerialPort $SniffPort, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.DtrEnable = $false; $sp.RtsEnable = $false; $sp.ReadTimeout = 1000
$sw = [System.IO.StreamWriter]::new($log, $false)
try {
  $sp.Open()
  $deadline = (Get-Date).AddSeconds($CaptureSeconds)
  while ((Get-Date) -lt $deadline) {
    try { $sw.WriteLine($sp.ReadLine()) } catch [TimeoutException] { }
  }
} finally {
  $sw.Close(); if ($sp.IsOpen) { $sp.Close() }
}

# --- 4) verdict ---
Write-Output "[4/4] analyzing..."
python $analyzer --seq-only $log
$code = $LASTEXITCODE
if     ($code -eq 0) { Write-Output "`nSEQ GATE: PASS  (en_sys_seq=false honored — per-MAC counters independent)" }
elseif ($code -eq 1) { Write-Output "`nSEQ GATE: FAIL  (shared-counter signature — en_sys_seq REGRESSED)" }
else                 { Write-Output "`nSEQ GATE: INCONCLUSIVE  (no frames parsed — check channel match / wiring; log: $log)" }
Write-Output "reminder: both boards are in PROBE/SNIFF mode — reflash them with your normal decoy build."
exit $code
```

- [ ] **Step 2: Write `tools/seq_gate/README.md`**

Create `tools/seq_gate/README.md`:

```markdown
# seq_gate — post-flash `en_sys_seq` regression gate

Verifies an ESP32-C5 still honors `esp_wifi_80211_tx(..., en_sys_seq=false)` — the chip behavior the
whole Wi-Fi decoy defense depends on. With that flag, each synthetic phone (source MAC) carries its
own independent 802.11 sequence counter. If a future IDF/chip build silently ignores the flag and
stamps one shared hardware counter, every decoy becomes trivially linkable and nothing else would
tell you. Run this after an IDF/toolchain bump or before a deployment.

## Why a bench gate (not a field canary)

- The regression can only be introduced by a firmware/IDF change — it cannot appear mid-deployment.
  So the check belongs at flash time, on the bench, not as a continuous field canary.
- An ESP32-C5 in promiscuous mode does **not** receive its own injected frames (verified: 60 injected,
  0 self-received). On-air verification therefore needs a **second** board.

## How it works

One board injects probe requests pinned to a single channel; a second board sniffs that channel and
logs `sa=<mac> seq=<n>`. The analyzer flags the shared-counter signature: a run of ≥3 time-ordered
frames whose seq increments by +1 while the MAC changes. Both boards must be on the **same** channel
so the sniffer hears every frame (a hardware counter ticks on every TX regardless of channel; viewed
through one channel of a hopping injector, the +1 signature is hidden).

## Usage

1. Activate your ESP-IDF 5.5 environment (`. $IDF_PATH/export.ps1`) so `idf.py` is on PATH.
2. Connect two C5s; note their COM ports.

```powershell
# normal run -> expect PASS:
.\tools\seq_gate\run_seq_gate.ps1 -InjPort COM12 -SniffPort COM16

# simulated regression -> expect FAIL (proves the gate actually discriminates):
.\tools\seq_gate\run_seq_gate.ps1 -InjPort COM12 -SniffPort COM16 -Shared
```

Exit codes: `0` PASS, `1` FAIL, `2` inconclusive (no frames — channel mismatch/wiring), `3` build/flash/env error.

## After running

Both boards are left in `PROBE` / `SNIFF` mode — **reflash them with your normal decoy build** before
redeploying.

## Flags (all default-off; shipped decoy unaffected)

- `PROBE_FIX_CH=<ch>` — pin the injector to one channel (`main/probe.c`).
- `PROBE_FORCE_SHARED=1` — `en_sys_seq=true`, i.e. simulate the regression (`main/probe.c`).
- `SNIFF_FIXED_CH=<ch>` — park the sniffer on a channel (`main/sniff.c`).

See `docs/superpowers/specs/2026-07-15-en-sys-seq-gate-design.md` for the full design.
```

- [ ] **Step 3: Lint the runner for syntax (parse-only, no hardware)**

Run: `powershell -NoProfile -Command "$null = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path tools/seq_gate/run_seq_gate.ps1), [ref]$null, [ref]$e); if ($e) { $e; exit 1 } else { 'parse OK' }"`
Expected: `parse OK` (no parser errors). This catches syntax mistakes before the hardware run in Task 4.

- [ ] **Step 4: Commit**

```bash
git add tools/seq_gate/run_seq_gate.ps1 tools/seq_gate/README.md
git commit -m "$(printf 'feat(seq-gate): bench runner + README for the en_sys_seq gate\n\nFlashes a single-channel injector + a same-channel sniffer, captures\nserial via .NET SerialPort (no dependency on personal tooling, no\nhardcoded paths), runs sniff_analyze.py --seq-only, prints PASS/FAIL.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy')"
```

---

### Task 4: Hardware validation (PASS + FAIL on two C5s)

Prove the gate discriminates on real hardware. This is the acceptance step — a gate that only ever shows PASS is untested and worthless.

**Files:**
- None (validation run). Optionally record results in `private/` (gitignored) if useful.

**Interfaces:**
- Consumes: the runner from Task 3, two esp32c5 boards on known COM ports.

- [ ] **Step 1: Confirm two C5s enumerate**

Run: `powershell -NoProfile -Command "Get-PnpDevice -PresentOnly -Class Ports | Where-Object FriendlyName -match 'CH343' | Select-Object FriendlyName"`
Expected: two `USB-Enhanced-SERIAL CH343 (COMx)` entries. Note the two COM numbers for `-InjPort` and `-SniffPort`. (If only one enumerates, this is the known USB-serial cycling — shuffle/replug until both appear.)

- [ ] **Step 2: Run the PASS path**

Activate the ESP-IDF 5.5 env, then run (substitute the two COM ports from Step 1):
`.\tools\seq_gate\run_seq_gate.ps1 -InjPort <COM_A> -SniffPort <COM_B>`
Expected: ends with `SEQ GATE: PASS`. If it prints `INCONCLUSIVE` (no frames), the sniffer heard nothing — re-check both boards enumerated and retry (the injector needs a few seconds of steady-state after its flash-reset).

- [ ] **Step 3: Run the FAIL path (simulated regression)**

Run: `.\tools\seq_gate\run_seq_gate.ps1 -InjPort <COM_A> -SniffPort <COM_B> -Shared`
Expected: ends with `SEQ GATE: FAIL` (the `en_sys_seq=true` shared counter produces the cross-MAC +1 run the analyzer catches). This is the proof the gate actually discriminates.

- [ ] **Step 4: Restore the two C5s to the shipped decoy firmware**

For each of the two ports, reflash the normal decoy (fleet + battery telemetry) so they rejoin the fleet:
Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Fleet -Vbat -Port <COM_A> -Do all -ReadSeconds 6 -Grep 'coexist|decoy alive'`
Then the same for `<COM_B>`.
Expected: each boots into `coexist ...` (normal decoy). Both boards are back to being decoys.

- [ ] **Step 5: Record the outcome**

Note in the session (and update the relevant memory) that the gate was validated: PASS on the normal path, FAIL on `-Shared`, both C5s restored to the decoy build. No commit needed (validation produced no repo changes).

---

## Notes for the executor

- **Firmware "tests" are builds.** ESP-IDF firmware flags can't be unit-tested on the host; Task 1's verification is a successful compile of both the default and gate configurations. The real behavioral proof is Task 4 on hardware.
- **Sticky CMake cache** is a known trap here: `-D` flags add cache defines but never remove them, so a cached `SIMULACRA_PROBE=1` would leak into the sniffer build (making the "sniffer" run PROBE mode, since `SIMULACRA_PROBE` early-returns first in `app_main`). The runner does a full `Remove-Item build,sdkconfig` between the two builds for exactly this reason — do not "optimize" it away.
- **Do not push.** Commit locally only.
