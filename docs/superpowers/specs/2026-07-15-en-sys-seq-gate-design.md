# Post-flash `en_sys_seq` regression gate — design

**Date:** 2026-07-15
**Status:** approved, ready for implementation plan

## Problem

The entire Wi-Fi side of the decoy defense rests on one fragile chip behavior:
`esp_wifi_80211_tx(WIFI_IF_STA, frame, len, /*en_sys_seq=*/false)`. With `en_sys_seq=false` the
driver transmits the 802.11 sequence-control field **exactly as we wrote it**, so each synthetic
phone (source MAC) carries its **own independent** sequence counter — indistinguishable from real
phones. If a future ESP-IDF or chip-firmware update silently ignores that flag and stamps **one
shared hardware counter** across all injected frames, every decoy regresses to trivially linkable
(a single monotonic counter spanning many MACs) and **nothing would tell us** — the decoy still
boots, still injects, still reports healthy. False confidence.

We want a check that reliably catches that regression.

## Key findings that shaped the design

1. **Self-RX is impossible on this silicon.** Verified 2026-07-15 with a throwaway spike: an
   ESP32-C5 in promiscuous mode injecting 60 frames received **0** of its own frames. The MAC
   filters self-TX. So a decoy cannot verify its own on-air sequence behavior — any "self-sniff at
   boot" canary would see nothing and always report PASS (the worst outcome). On-air verification
   **requires a second board** sniffing the injector.

2. **The regression is a build-time event.** The chip cannot flip its own `en_sys_seq` semantics
   mid-deployment; the behavior only changes when the firmware/IDF changes. So the regression window
   is **flash time**, not runtime. The right place for the check is the bench, right after flashing —
   not a continuous field canary paying density/complexity to watch for something that can't happen
   in the field.

3. **The discriminator already exists.** `tools/probe_audit/analyzers/sniff_analyze.py` already
   implements the exact detector: *FAIL if any run of ≥3 time-ordered frames increments seq by +1
   while the source MAC changes* (= one shared hardware counter). It already exits `0`=PASS / `1`=FAIL.

4. **Single-channel is mandatory.** A hardware counter ticks on **every** TX regardless of channel.
   If the sniffer only hears a subset of the injector's frames (injector hopping 1/6/11 + 5 GHz), a
   real regression appears as *large seq gaps*, not +1 runs, and the detector **misses it (false
   PASS)**. Both boards must be pinned to one channel so the sniffer sees every frame and the
   shared-counter +1 signature is intact.

## Design

A two-board bench gate, run after flashing, that emits a PASS/FAIL verdict on per-MAC sequence
independence.

### Board A — injector

Flashed in the existing `SIMULACRA_PROBE` Wi-Fi-only mode, plus two new **default-off** build flags
added to `main/probe.c`:

- **`PROBE_FIX_CH`** (default `0` = hop): when set to a channel number, `probe_task` pins injection
  to that single channel every burst instead of hopping. Ensures Board B (parked on the same channel)
  hears every injected frame.
- **`PROBE_FORCE_SHARED`** (default `0`): when `1`, `probe_inject_burst` calls
  `esp_wifi_80211_tx(..., /*en_sys_seq=*/true)` — deliberately invoking the shared hardware counter
  to **simulate the regression**. Used only to validate that the gate catches a real FAIL. Never
  shipped.

Both flags default off, so the shipped decoy firmware is byte-for-byte unaffected. Wired into the
`main/CMakeLists.txt` `foreach` flag-forwarding block.

### Board B — sniffer

The existing `SIMULACRA_SNIFF` mode. It already parks on `SNIFF_FIXED_CH` (default ch1) and logs one
line per probe request: `pr sa=<mac> seq=<n> rssi=<d>`. The only change: make `SNIFF_FIXED_CH` a
forwardable build flag (`#ifndef` default + added to the CMakeLists `foreach`), so the runner drives
**both** boards' channel from one `-Channel` argument — Board A's `PROBE_FIX_CH` and Board B's
`SNIFF_FIXED_CH` are guaranteed to match. If they didn't, the sniffer would hear nothing and the gate
would (correctly, but unhelpfully) report "no frames parsed" (exit 2).

### Analyzer

Add a **`--seq-only`** flag to `sniff_analyze.py`. The file already runs three checks
(seq-independence, decorrelation, constellation); checks 2 and 3 are probe-*realism* metrics that
are irrelevant to this gate and would false-FAIL on a short bench capture. `--seq-only` runs **only**
the seq-independence check and sets the exit code from it alone. Existing exit semantics preserved:
`0`=PASS, `1`=FAIL, `2`=no frames parsed.

### Runner

New `tools/seq_gate/run_seq_gate.ps1 -InjPort <COM> -SniffPort <COM> [-Shared] [-Channel 1]`:

1. Build + flash Board A on `-InjPort`: `SIMULACRA_PROBE=1`, `PROBE_FIX_CH=<Channel>`, and
   `PROBE_FORCE_SHARED=1` iff `-Shared`.
2. Build + flash Board B on `-SniffPort`: `SIMULACRA_SNIFF=1`.
3. Capture `-SniffPort` serial for ~30 s to a log (reuses `read_serial.py`).
4. Run `python sniff_analyze.py --seq-only <log>`; surface its stdout and exit code.
5. Print a clear `SEQ GATE: PASS` / `SEQ GATE: FAIL` line and exit with the analyzer's code.

Reuses `build-flash-read/build_flash_read.ps1` and `read_serial.py`; no new capture or build
machinery.

## Data flow

```
Board A (SIMULACRA_PROBE, PROBE_FIX_CH=1)          Board B (SIMULACRA_SNIFF, ch1 parked)
  probe_inject_burst on ch1                           promiscuous rx_cb
  N distinct SAs, per-agent seq       --- RF ch1 --->  logs: pr sa=.. seq=.. rssi=..
  esp_wifi_80211_tx(..., en_sys_seq=false)                    |
                                                              v  serial
                                                    read_serial.py -> log
                                                              |
                                                              v
                                              sniff_analyze.py --seq-only
                                                              |
                                                  cross-MAC +1 run >= 3 ?
                                                     no -> PASS   yes -> FAIL
```

## Validation (acceptance)

Run on real hardware, two C5s on the bench:

- **PASS path:** normal run (`en_sys_seq=false`) → analyzer reports seq-independence PASS, runner
  exits 0.
- **FAIL path:** `-Shared` run (`en_sys_seq=true`, the simulated regression) → analyzer detects the
  cross-MAC +1 run and reports FAIL, runner exits 1.

Both verdicts must be demonstrated. A gate that only ever shows PASS is untested and worthless; the
`-Shared` path is what proves it actually discriminates.

## Scope / footprint

- Firmware: ~15 lines in `main/probe.c` + one-line `#ifndef` guard in `main/sniff.c` +
  `main/CMakeLists.txt` flag forwarding, all behind default-off / same-default flags. No
  shipped-behavior change, no density cost, no wire-struct change.
- Python: ~5 lines (`--seq-only`) in `sniff_analyze.py`.
- New: `tools/seq_gate/run_seq_gate.ps1`, `tools/seq_gate/README.md`.
- Docs: one line in the post-flash checklist (`private/` runbook) pointing at the gate.

## Explicit non-goals (YAGNI)

- **No on-fleet / runtime canary.** The regression can't appear in the field; a continuous canary
  spends density/complexity for nothing.
- **No new dedicated firmware app-mode.** Board A reuses `SIMULACRA_PROBE`; the gate tests the
  IDF+chip behavior, which is app-independent.
- **No automatic CI integration.** It's a manual bench gate run after an IDF/toolchain bump or before
  a deployment; hands-on by design.

## Honest caveat (to document in the README)

Board A runs in `SIMULACRA_PROBE` mode, not the exact shipped decoy binary. This is sound: honoring
`en_sys_seq=false` is a property of the **IDF + chip**, not the application — both the probe mode and
the shipped decoy call the identical API on the identical IDF build. The gate answers "does this
IDF/chip still honor `en_sys_seq=false`," which is precisely the regression risk.
