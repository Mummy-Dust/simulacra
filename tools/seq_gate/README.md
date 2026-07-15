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
