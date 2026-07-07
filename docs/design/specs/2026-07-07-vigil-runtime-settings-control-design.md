# Vigil runtime settings + control — design

**Date:** 2026-07-07
**Status:** design (approved for planning)
**Depends on:** existing ESP-NOW `radar_wire` link, `churn` engine runtime setters, web-UI status/control backend, `fleet` self-exclusion.

## Summary

Turn Vigil (the CYD display node) from a read-only radar into a **fleet control console**:
let the user pick a behaviour **preset** on the touchscreen and push it to every decoy over
ESP-NOW. Underneath, promote the decoy's compile-time churn knobs to a **runtime settings
backend** (RAM + NVS) shared by both the web UI (granular per-value control) and the incoming
CONFIG frames (presets). Because this changes the ESP-NOW link from *telemetry-only* to
*control*, CONFIG frames are authenticated with an **Ed25519 signature** (capture-resistant)
on top of the existing AES-256-GCM seal, and every applied value is **clamped to a safe floor**
so a forged or replayed command can never zero out a decoy or disable detection.

## Goals

- A runtime settings struct on the decoy that drives churn/coexist behaviour, persisted to NVS
  so changes survive reboot.
- Dwell/cooldown (today `#define`s in `churn.h`) become runtime-mutable; `active_target`,
  `paused`, `accel` already are.
- One backend, two writers: the **web UI** (granular) and **CONFIG frames from Vigil** (presets).
- A five-rung preset ladder — **PAUSE / STEALTH / NORMAL / DENSE / MAX** — carrying *intent*;
  each decoy maps a preset to concrete values against **its own** ceiling (`CHURN_ACTIVE_SET`),
  so one preset works across a heterogeneous Ward/Shade fleet.
- A button-based **CONTROL page** on Vigil: `◀ [ PRESET ] ▶` selector + explicit **SEND** button.
- **Ed25519-signed** CONFIG frames + **safe-bounds clamp** on the decoy.
- Vigil **broadcasts** the preset to all decoys at once.

## Non-goals (this slice)

- Per-node addressing / a device picker on Vigil (broadcast only).
- Granular value spinners on the Vigil touchscreen (presets only there; granular lives in web UI).
- OTA, remote reboot, or any control beyond the churn/coexist behaviour knobs.
- Changing the read (telemetry) trust model — STATUS/telemetry stays PSK-only as today.

## Threat model & security

Today the link is read-only by design: a leaked PSK or a spoofed Vigil can only *eavesdrop*.
Adding control means a forged CONFIG could quietly degrade a decoy (churn→0, detection off),
which is worse than eavesdropping. Two independent layers bound this:

1. **Ed25519 command authenticity (capture-resistant).**
   Vigil holds a **private** signing key; decoys hold only Vigil's **public** key. A CONFIG
   frame is GCM-sealed as today (confidentiality + PSK-auth + replay gate) **and** carries a
   64-byte Ed25519 signature over `nonce ‖ command`. The decoy GCM-opens the frame, passes the
   replay gate, then **verifies the signature** before applying. Because decoys never hold the
   private key, dumping the flash of any captured decoy does **not** grant command authority —
   the property a second *symmetric* key could not provide.
   - **Signing over `nonce ‖ command`** (not the command alone) binds the signature to this
     specific sealed frame. Without this binding, a PSK holder could lift Vigil's signed command
     out of an old frame and re-seal it under a fresh nonce/counter to bypass the replay gate.
     Binding to the nonce makes any re-seal invalidate the signature.

2. **Safe-bounds clamp (blast-radius limiter).**
   Crypto stops *forgery*; the clamp bounds the damage if crypto is ever defeated. Every value
   the settings backend applies is clamped to a safe floor: crowd size cannot drop below the
   STEALTH floor, churn cannot be zeroed, detection cannot be disabled *via CONFIG*. The presets
   all live inside the safe range, so the clamp is invisible in normal use and only ever rejects
   hostile/malformed input. The clamp guards the **floor**; the preset you send sets the
   **ceiling** (up to each board's `CHURN_ACTIVE_SET`).

Layering summary: **GCM** = only-PSK-holders, confidential, no replay. **Ed25519** = only-Vigil
authored, survives decoy capture. **Clamp** = even a defeated command can't cross safe bounds.

### Key provisioning

- A one-time offline step generates an Ed25519 keypair (a small host script committed under
  `tools/`; the private key is **not** committed).
- The **public** key is baked into decoy builds (a generated header, mirroring how
  `radar_key.h` carries the PSK). The **private** key is baked into the Vigil build only.
- Gated behind a build `#define` (e.g. `SIMULACRA_CONFIG_CTRL`) so control support is opt-in at
  compile time and boards without the key material simply ignore CONFIG frames.

## Architecture

```
                 Vigil (CYD)                              Decoy (Ward/Shade)
        ┌───────────────────────────┐            ┌──────────────────────────────┐
 touch →│ CONTROL page (Option A)   │            │ esp_now_link on_recv         │
  x,y   │  ◀ [PRESET] ▶  [ SEND ]   │  ESP-NOW   │  type==CONFIG:                │
        │  select + confirm         │  CONFIG 7  │   GCM-open → replay gate      │
        │  sign(nonce‖cmd, privkey) │──broadcast→│   → Ed25519 verify (pubkey)   │
        │  GCM-seal → send          │            │   → settings_apply_preset()   │
        └───────────────────────────┘            │        │                      │
                                                 │        ▼                      │
                                                 │  settings backend (RAM)       │
                                                 │   map preset→values + CLAMP   │
                                                 │   ├─ churn setters             │
                                                 │   ├─ coexist (probe cadence)   │
                                                 │   └─ NVS persist (debounced)   │
                                                 │        ▲                      │
                                                 │   web UI (granular writes)     │
                                                 └──────────────────────────────┘
```

### Components

**Decoy side (`main/`)**

- **`settings.{h,c}`** — the runtime settings backend. Owns a `sim_settings_t` (active_target,
  dwell_min/max, cooldown_min/max, accel, paused, …), the **preset→values map**, the **clamp**,
  the **apply path** into churn/coexist, and **NVS load/save** (debounced write on change,
  load at boot). Single source of truth; both writers go through it.
  - `settings_init(void)` — load from NVS (or firmware defaults), apply.
  - `settings_apply_preset(sim_preset_t p)` — map→clamp→apply→persist.
  - `settings_set_field(...)` / getters — granular path for the web UI.
  - `settings_get(sim_settings_t *out)` — snapshot for status/UI.
- **`churn.{h,c}`** — add runtime setters for dwell min/max and cooldown min/max (mirroring the
  existing `churn_set_active_target/paused/accel`), backed by RAM state that the tick loop reads
  instead of the `#define`s. The `#define`s remain as the **firmware defaults**.
- **`config_wire.{h,c}`** (shared component `simulacra_radar/`) — `RADAR_TYPE_CONFIG 7`, the
  packed `config_cmd_t { uint8_t version; uint8_t preset_id; }` (room to grow), and
  pack/unpack + the Ed25519 sign (Vigil) / verify (decoy) helpers over `nonce ‖ cmd`.
- **`esp_now_link.c`** — handle `RADAR_TYPE_CONFIG`: replay gate (new `radar_replay_t`), Ed25519
  verify, then `settings_apply_preset()`. Reject silently on any failure (bad sig / replay /
  unknown preset), matching the existing silent-drop style.
- **Web UI** — route its existing pause/detection controls through `settings.*`, and expose the
  newly-runtime knobs (dwell/cooldown) + a preset selector for parity.

**Vigil side (`cyd/main/` + shared renderer)**

- **`radar_ui.{h,c}`** — add `RADAR_VIEW_CONTROL` to the view enum (before `_COUNT`), so it joins
  the tap-to-cycle rotation. Existing idle-return-to-radar applies (leaving the control page
  after inactivity is fine; in-progress selection simply resets — acceptable, re-enter to retry).
- **Touch layer** — extend beyond today's boolean `touched()` to report **x,y** from the XPT2046,
  and add simple hit-testing (left/right selector zones + SEND button rect) used **only** on the
  CONTROL page. Other pages keep "tap anywhere = next view".
- **`radar_render.{h,c}`** — add `draw_control()` rendering the `◀ [PRESET] ▶` selector, the
  SEND button, and a transient "SENT ✓" flash. Fed by a small `radar_ctrl_info_t`
  { selected_preset, send_flash_ms_left }.
- **CONFIG send** — Vigil seals a `config_cmd_t{preset}` as `RADAR_TYPE_CONFIG`, signs
  `nonce ‖ cmd` with its private key, broadcasts (3–4×, matching the request pattern).

### Preset ladder (semantics; each decoy clamps to its own ceiling)

| Preset  | Concurrent crowd        | Turnover / probes                  | Intent                    |
|---------|-------------------------|------------------------------------|---------------------------|
| PAUSE   | frozen (hold adverts)   | churn paused                       | go dark / hold            |
| STEALTH | ~40% of ceiling (floor) | long dwell, minimal probe cadence  | subtle / low-power        |
| NORMAL  | full (ceiling)          | firmware-default dwell/cadence     | everyday                  |
| DENSE   | full                    | shorter dwell, higher cadence      | blend into a busy area    |
| MAX     | full                    | shortest safe dwell, max cadence   | maximum air presence      |

- Presets carry **intent**; the decoy resolves them to concrete values against its own
  `CHURN_ACTIVE_SET` and clamps. STEALTH's crowd size *is* the clamp floor — nothing (forged or
  otherwise) can push a decoy quieter than STEALTH.
- MAX is the honest trade-off button: most air presence, most detectable, fastest battery drain
  (matters most on the mobile C6). The SEND button is its confirm — no special-casing needed.

## Data flow

**Vigil → decoy (control):**
1. User taps CONTROL page selector to highlight a preset, taps **SEND**.
2. Vigil builds `config_cmd_t{version,preset}`, picks nonce (salt‖++counter), signs `nonce‖cmd`
   with private key, GCM-seals as `RADAR_TYPE_CONFIG`, broadcasts 3–4×. Renders "SENT ✓".
3. Each decoy GCM-opens → replay gate → Ed25519 verify → `settings_apply_preset()` →
   map→clamp→apply→NVS persist. Next STATUS reply reflects the new `active_target`/flags, so the
   change is visible back on Vigil's STATS/radar pages (closing the loop without a dedicated ACK).

**Web UI → decoy (granular):** browser control → `settings_set_field()` → clamp→apply→persist.

**Boot:** `settings_init()` loads NVS (or firmware defaults) and applies before churn starts.

## Error handling

- CONFIG frame fails GCM-open, replay gate, **or** Ed25519 verify → **silently dropped** (no
  state change, no log leak of control attempts beyond existing debug tags).
- Unknown/out-of-range `preset_id` or `version` → rejected by the map; no apply.
- Clamp always runs on apply, so even an accepted-but-extreme value lands in safe bounds.
- NVS write failure → keep RAM state applied (behaviour correct for this session), log a warn;
  do not block on persistence.
- Vigil built **without** the private key (`SIMULACRA_CONFIG_CTRL` off) → CONTROL page hidden
  from the view cycle (no dead button).

## Testing

- **Pure/host-testable (in `churn_selftest.c`):**
  - Preset→values map produces the expected concrete values per preset.
  - Clamp floor: a below-STEALTH / zeroing / detection-off request is clamped up, never applied
    as given.
  - Unknown preset id / bad version → rejected, no state mutation.
  - `config_cmd_t` pack/unpack round-trips.
  - Ed25519 sign-then-verify round-trip with a fixed test keypair; a tampered `cmd` or a
    tampered `nonce` fails verification (proves the nonce binding).
- **On-target:**
  - Two-board: Vigil SEND of each preset → decoy applies (observe `active_target`/dwell change in
    STATUS + serial log), survives reboot (NVS).
  - Forged/tampered CONFIG (flip a signature byte) → decoy ignores.
  - Heterogeneous ceiling: MAX on a C6 clamps to its lower ceiling, on C5 fills to 16.
  - Web UI granular change and a Vigil preset both reflected consistently (one backend).

## Rollout / gating

- All control behaviour behind `SIMULACRA_CONFIG_CTRL` (compile-time). Decoys without the public
  key ignore CONFIG; Vigil without the private key hides the CONTROL page. The rest of the fleet
  (telemetry, learn/sig sync, fleet MACs) is unaffected.
- Frame type `7` is the next free id (1 REQUEST, 2 STATUS, 3 LEARN_OFFER, 4 LEARN_SYNC,
  5 SIG_SYNC, 6 FLEET_MACS).

## Open questions / deferred

- Exact concrete values for each preset's dwell/cooldown/cadence — tuned during implementation
  against the current firmware defaults, then frozen in the map.
- Whether the web UI also surfaces a live read-back of "last preset applied / by whom" — nice,
  not required for this slice.
- Per-node addressing and a control-capability audit log are future extensions if the fleet grows
  beyond a handful of boards.
