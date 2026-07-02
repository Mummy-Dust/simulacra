# Remote ESP-NOW display (CYD) — design

> A "threat radar" screen for the Simulacra decoy that lives on a *separate* Cheap Yellow
> Display (ESP32-2432S028) instead of a panel wired to the C5/C6. The decoy answers on-demand
> telemetry requests over ESP-NOW; the CYD renders the radar. Complements — does not replace —
> the on-board wired-panel display (`docs/design/specs/2026-07-01-on-device-display-design.md`),
> which stays parked pending its ST7789 panels. Both reuse one shared radar core.

## Goal

Give the screenless decoy a glanceable, *physically decoupled* readout: the C5/C6 decoy can be
hidden (bag, vehicle, fixed install) while a handheld/desk CYD shows live follower-detection +
decoy telemetry — without turning the anti-tracking device into a linkable RF beacon.

## Why receiver-only on the CYD

The CYD is a classic **ESP32-D0WD-V3** (verified on the attached unit: dual-core Xtensa, Wi-Fi +
BT **4.2**, 4 MB flash). BLE 4.2 has **no extended advertising**, which the entire churn engine
depends on (4 concurrent ext-adv instances — the same reason the ESP32-S3 was rejected). So the
CYD **cannot run the decoy**; it is a display/receiver only. The decoy stays on the C5/C6.

## Locked decisions

- **Roles:** decoy (C5/C6) = sender/responder; CYD = receiver + renderer. One-way telemetry
  (decoy→CYD), triggered by CYD→decoy requests.
- **Trigger:** **CYD polls, decoy answers.** The decoy is silent (listen-only) until it hears a
  valid REQUEST, then broadcasts a short STATUS burst and goes quiet. Emission happens only while
  the user is actively looking at the CYD.
- **Toolchain:** the CYD firmware is **ESP-IDF** (target `esp32`) — same toolchain as the rest of
  the project (build-flash-read covers it) and it reuses the shared radar core verbatim. Arduino
  is the documented fallback if CYD panel bring-up fights us.
- **Transport:** ESP-NOW, **broadcast-addressed**, on a **fixed channel (1)** chosen from the
  probe hop set {1,6,11} so the hopping decoy visits it regularly.
- **Security:** **AES-GCM** (encrypt + authenticate) under a 32-byte compile-time pre-shared key
  `SIMULACRA_ESPNOW_KEY` (placeholder default the user replaces; never a real key in the repo),
  with a per-frame nonce and bidirectional replay rejection.
- **Payload:** the whole snapshot fits one ESP-NOW frame (~151 B ≤ 250 B) — no fragmentation.
- **Opsec:** silent-by-default, broadcast (no linkable MAC pair), random locally-administered
  source MAC (per boot), on-demand/low-duty. Law 3 inherited — aggregates + hash-only threats.
- **Gating:** decoy side behind `SIMULACRA_ESPNOW` (source `#define`, default **0**), independent
  of the on-board-display gate `SIMULACRA_DISPLAY`. The CYD app is inherently the display.
- **Distribution:** committed locally, not pushed (same discipline as the rest of the tree).

## Architecture

Two firmwares over one shared pure core.

### Shared component — `components/simulacra_radar/`
Portable, mostly-pure code compiled by **both** firmwares, the single source of truth:
- radar geometry (`rssi→radius`, `hash→angle`, `polar→xy`) + the view/backlight state machine
  (relocated here from the parked on-board-display plan's Tasks 1–2);
- gfx primitives (band-buffer pixel/line/circle/fill/text) + an 8×8 font + the three view
  renderers. These are hardware-agnostic — only the final `esp_lcd_panel_draw_bitmap` flush is
  panel-specific, and that API is identical across controllers;
- the wire layer: `radar_wire_status_t`, pack/unpack, AES-GCM seal/open, and the replay check.

The renderers operate on `radar_wire_status_t` as the **canonical view-model**. The decoy converts
its `webui_status_t` → `radar_wire_status_t` (one function) for both local rendering and
transmission, so the web UI, the on-board panel, and ESP-NOW all derive from a single
`webui_gather_status()` snapshot.

### Decoy side (C5/C6) — `main/esp_now_link.{h,c}` (new)
Gated `SIMULACRA_ESPNOW` (default 0). Inits ESP-NOW on top of the Wi-Fi the decoy already runs
(after `coexist_set_wifi_enabled(true)`, so it never races bring-up), registers a broadcast peer on
channel 1, and installs an RX callback. Listen-only until a valid REQUEST arrives; then a tiny
response task gathers status, converts + packs + seals it, and **broadcasts 3 STATUS frames
back-to-back within the current channel-1 dwell** (redundancy against loss; never fights the hop
schedule). The RX callback only validates + notifies — no heavy work in Wi-Fi task context.

### CYD side — `cyd/` (new ESP-IDF app, target `esp32`)
Its own `CMakeLists.txt` + `sdkconfig.defaults`, pulling the shared component via
`EXTRA_COMPONENT_DIRS`. Provides the board seam: panel init (ILI9341 **or** ST7789 — TBD by a
30-second probe flash; XPT2046 touch; backlight GPIO21; display and touch on *separate* SPI buses;
likely BGR + color inversion), touch input, and the ESP-NOW receiver (locks channel 1, listens for
STATUS, decrypts/verifies, feeds the shared renderer). Touch triggers a REQUEST and pages the
views. A **link-freshness indicator** shows "updated Ns ago" / "searching…" / "no decoy in range"
based on the last valid STATUS.

## Request/response flow

```
CYD:   [touch] -> broadcast REQUEST (AES-GCM, carries a client nonce) x3-5 over ~1s
Decoy: (hopping 1/6/11; hears REQUEST only while on ch1)
       valid REQUEST -> gather status -> broadcast STATUS x3 back-to-back (same dwell) -> silent
CYD:   STATUS -> decrypt+verify+replay-check -> render radar; stamp "updated now"
       no STATUS within timeout -> "searching…"; longer -> "no decoy in range" (dim)
```

Because the decoy only catches a request while its hop is on ch1, the CYD repeats the request for
a second or two; first-response latency is a few seconds at most — acceptable for a glance, and no
dwell change is forced on the probe injector.

## Wire format (one frame, ≤250 B)

```
[ magic(2)=0x5A4D | ver(1) | type(1) | nonce(12) | ciphertext(N) | tag(16) ]
```
- `magic|ver|type` are plaintext but **authenticated as GCM AAD** (tampered header → tag fails).
- `nonce` = `[per-boot random salt(4) | monotonic counter(8)]` — unique per (key, message).
- `ciphertext` = AES-GCM(payload): STATUS → the packed status; REQUEST → a 4-byte client nonce
  the STATUS echoes (matches reply to a fresh request).
- `tag` (16 B) = GCM auth tag.

Packed status payload (explicit, fixed-width, little-endian — both chips are LE, no byte-swap;
never send `webui_status_t` directly as its padding isn't portable):
```c
typedef struct __attribute__((packed)) {
    uint32_t uptime_s; uint8_t flags;            // bit0 paused, bit1 config_mode
    uint16_t active_devices, roster_size; uint32_t probes_sent;
    uint16_t epoch, pop_ewma; uint32_t total_obs;
    uint8_t active_target, threat_count;
    struct __attribute__((packed)) {             // hash-only (Law 3)
        uint32_t hash; uint16_t vendor; uint8_t epochs; int8_t best_rssi;
        uint16_t first_epoch, last_epoch;
    } threats[DETECT_MAX_THREATS];
} radar_wire_status_t;                            // 119 B; frame total ~151 B
```

## Replay protection

Each side tracks the highest `(salt, counter)` accepted from the peer role and rejects anything
not strictly newer; a changed `salt` means the peer rebooted and resets the expectation. This
covers both directions — an attacker can neither replay an old STATUS to the CYD nor replay a
captured REQUEST to force the decoy to emit.

## Opsec mechanics

- **Silent-by-default** — decoy transmits only in response to a valid request; no idle beacon.
- **Broadcast-addressed** (FF:FF:FF…) — no linkable decoy↔CYD MAC pair.
- **Random locally-administered source MAC**, set per boot (per-burst rotation is optional later
  hardening; on-demand + brief bursts already keep exposure tiny).
- **Requests originate from the CYD**, not the protected device — the decoy is a pure listener that
  emits only when you're actively looking.

## Testing

- **Shared pure self-tests** (in the decoy's `churn_selftest`, `ST_CHECK`, run on the attached C5):
  geometry, the view/FSM, wire pack/unpack, and AES-GCM **seal→open round-trip + tamper-fails +
  stale-counter-rejected**. Full headless coverage of the protocol + math before any radio.
- **Decoy on-target:** responds only to a valid request, encrypted, silent otherwise (verify with
  the CYD or a sniff); no watchdog; decoy keeps churning + injecting throughout.
- **CYD on-target:** renders injected test data; then touch → REQUEST → live radar from the decoy;
  freshness states behave; a walked-in known tracker appears as a follower dot at the right ring.
- **End-to-end:** the full loop on the real CYD (COM14) + real decoy (C5 COM12 / SparkFun C6 COM13).

## Scope & sequencing

One spec; the plan sequences it so each stage is independently verifiable:
1. Shared `simulacra_radar` component + self-tests (geometry, FSM, wire, AES-GCM).
2. Decoy `esp_now_link` responder + `SIMULACRA_ESPNOW` gate + boot wiring (on-target: responds).
3. CYD ESP-IDF app: panel + touch bring-up, then ESP-NOW receiver + render (on-target).
4. End-to-end acceptance + link-freshness UX.

## Known limitations (accepted for this milestone)

- **A few-seconds first-response latency** (decoy catches requests only during its ch1 dwell).
- **Pre-shared key** (no key exchange / rotation) — a compile-time secret shared by decoy + CYD.
- **CYD panel controller (ILI9341 vs ST7789) unconfirmed** until a probe flash; both handled by
  `esp_lcd`, only the init/pin details differ.
- **Per-boot (not per-burst) source-MAC rotation** — sufficient given on-demand brief emission.
- Bearing on the radar remains synthetic (inherited from the on-board-display design), labeled.
