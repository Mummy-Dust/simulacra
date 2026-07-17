```
root@simulacra:~# ./summon --crowd --bury-signal
  ██████  ██▓ ███▄ ▄███▓ █    ██  ██▓    ▄▄▄       ▄████▄   ██▀███   ▄▄▄
▒██    ▒ ▓██▒▓██▒▀█▀ ██▒ ██  ▓██▒▓██▒   ▒████▄    ▒██▀ ▀█  ▓██ ▒ ██▒▒████▄
░ ▓██▄   ▒██▒▓██    ▓██░▓██  ▒██░▒██░   ▒██  ▀█▄  ▒▓█    ▄ ▓██ ░▄█ ▒▒██  ▀█▄
  ▒   ██▒░██░▒██    ▒██ ▓▓█  ░██░▒██░   ░██▄▄▄▄██ ▒▓▓▄ ▄██▒▒██▀▀█▄  ░██▄▄▄▄██
▒██████▒▒░██░▒██▒   ░██▒▒▒█████▓ ░██████▒▓█   ▓██▒▒ ▓███▀ ░░██▓ ▒██▒ ▓█   ▓██▒
▒ ▒▓▒ ▒ ░░▓  ░ ▒░   ░  ░░▒▓▒ ▒ ▒ ░ ▒░▓  ░▒▒   ▓▒█░░ ░▒ ▒  ░░ ▒▓ ░▒▓░ ▒▒   ▓▒█░
░ ░▒  ░ ░ ▒ ░░  ░      ░░░▒░ ░ ░ ░ ░ ▒  ░ ▒   ▒▒ ░  ░  ▒     ░▒ ░ ▒░  ▒   ▒▒ ░
░  ░  ░   ▒ ░░      ░    ░░░ ░ ░   ░ ░    ░   ▒   ░          ░░   ░   ░   ▒
      ░   ░         ░      ░         ░  ░     ░  ░░ ░         ░           ░  ░
  summon the crowd · bury the signal              [ ble · wifi · esp-now ]
```

<p align="center">
  <img src="https://img.shields.io/badge/license-pending-8a8a8a?style=flat-square" alt="License pending">
  <img src="https://img.shields.io/badge/platform-ESP32--C5%20·%20C6%20·%20CYD-9d4edd?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/ESP--IDF-5.4%20·%205.5-1f9e6a?style=flat-square" alt="ESP-IDF">
  <img src="https://img.shields.io/badge/radios-BLE%20·%20Wi--Fi-14a06a?style=flat-square" alt="Radios">
</p>

**A modular, multi-node ESP32 anti-tracking system.** Simulacra continuously fabricates a
churning crowd of plausible-but-fake wireless devices around you — drowning your real devices in
noise so that passive trackers, ALPR add-ons, and co-travel correlators can't reliably pick your
signal out of the crowd — while passively watching for the trackers that follow you.

It is built from cooperating nodes, each playing to a different board's strengths, coordinated
over an encrypted ESP-NOW link.

---

## ⚠️ Legal & responsible use

Simulacra is a research and personal-privacy tool. It transmits synthetic BLE/Wi-Fi advertising
traffic and observes the RF around you. **You are responsible for complying with the radio
regulations and laws in your jurisdiction.** Do not use it to harass, impersonate a specific
person or device, interfere with networks or emergency services, or evade lawful process. Use it
on hardware you own, in ways that are legal where you are. No warranty; see the license.

---

## How it works

- **A churning synthetic crowd.** Instead of hiding a device, Simulacra hides it *in a crowd* —
  generating a rotating population of realistic fake devices (random-static MACs, plausible
  vendor/format shapes, realistic advertising cadence) that constantly turns over.
- **Structure, never identity.** The self-learning engine harvests the *shape* of real nearby
  device adverts (vendor/format/AD structure) and turns them into new decoy archetypes — but it
  strips all identifying content. A learned template is "a device *of this kind*", never "*this
  device*". A hard "Law-3" gate refuses to ever learn or emit forbidden identity subtypes
  (e.g. Apple continuity / Fast Pair pairing beacons).
- **Passive detection.** While it churns, it watches for followers — devices that persist with you
  — and matches adverts against a signature database of known trackers (AirTag / SmartTag / Tile)
  and surveillance gear.
- **Coordinated, not cloned.** Nodes share a learned library and exclude each other from their own
  models over an authenticated ESP-NOW link, so the fleet behaves like one diverse crowd rather
  than several identical decoys.

## Architecture — the nodes

| Node | Board | Role |
|------|-------|------|
| **Ward** | ESP32-C5 (dual-band Wi-Fi 6) | Fixed / vehicle decoy — dense, dual-band crowd generation |
| **Shade** | ESP32-C6 | Mobile / everyday-carry decoy — lean, 2.4 GHz, low-profile |
| **Vigil** | ESP32 + "Cheap Yellow Display" | Controller & librarian — radar/status screen, touch control, encrypted SD library, fleet key custody |

Roles are selected at build time so one firmware tree serves every board.

## Features

- Rotating BLE decoy crowd with realistic random-static MACs, vendor/format shapes, and advertising
  cadence — including **persistent devices with per-type address rotation** (RPAs/NRPAs rotate on
  realistic schedules, static beacons hold) and a **death/rebirth lifecycle**, so the population
  turns over like a real crowd instead of a fixed set of decoys.
- **Wi-Fi PAN cover:** independent, archetype-faithful probe-request agents (iPhone / Galaxy /
  Pixel / generic Android). Each fake phone carries its **own 802.11 sequence counter**, so the
  real device can't be fingerprinted out of the probe traffic by its sequence/timing constellation.
- On-device **self-learning** of ambient device *shapes* into new decoy archetypes (structure-only,
  Law-3 gated), synced across the fleet and persisted to an **encrypted-at-rest** SD library on Vigil.
- **Passive follower detection** and **tracker/surveillance fingerprint** matching.
- **Signed fleet control:** Vigil pushes Ed25519-signed behaviour presets (PAUSE / STEALTH /
  NORMAL / DENSE / MAX) to every decoy over ESP-NOW.
- **On-air fleet enrollment (ECDH):** decoys ship with no shared transport key and enroll on-air via
  a mutually-authenticated 3-message handshake, so **capturing a decoy does not compromise the fleet.**
- **Vigil console:** live radar/threat display, touch-driven sigil dashboard, a per-node fleet
  roster, and enroll/revoke control for fleet members.
- **Fleet health at a glance:** decoys report TX self-health and battery state over the link, so
  Vigil surfaces a `DEGRADED` or `LOW BATT` node on its roster before it goes quiet in the field.

## Security model

- **Asymmetric by design.** The controller (Vigil) holds the private signing key; decoys hold only
  the public key. A captured decoy can verify commands but cannot forge them or control the fleet.
- **Never trust the wire.** Every synced/seeded template is re-gated (budget + Law-3 + hash recompute)
  on receipt, so a leaked key or spoofed node still cannot inject a forbidden identity.
- **Structure-only learning.** Real-world captures are stripped to skeletons; no bystander identities
  or names are stored or emitted.
- **Measured, not assumed.** A host-side audit suite compiles the *real* generator and scores how
  separable the decoys are from a real crowd across every axis an adversary could use — address type,
  advertising interval, vendor mix, AD structure, presence/lifespan, and RSSI — turning "are the
  decoys convincing?" into a single regression-gate number instead of a hope.

## Hardware

- **ESP32-C5** (Ward) and **ESP32-C6** (Shade) decoys.
- **ESP32** "Cheap Yellow Display" (ILI9341 + XPT2046 touch + microSD) for Vigil.

## Build & flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) — v5.5 for the C5/C6 decoys,
v5.4 for the Vigil (classic ESP32). With the IDF environment active:

```sh
# Decoy (Ward / Shade) — from the repo root
idf.py set-target esp32c5          # or esp32c6 for Shade
idf.py -DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build
idf.py -p <PORT> flash monitor

# Vigil controller — from ./cyd
cd cyd
idf.py set-target esp32
idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build
idf.py -p <PORT> flash monitor
```

Build-time gates (`-D…=1`) select each node's role and optional subsystems (fleet control,
enrollment, self-test). See `docs/` for design specs and per-feature notes.

**Fleet-key regime must match across the whole fleet.** With `-DSIMULACRA_FLEET_PROVISION=1`
(shown above) the Vigil mints a random fleet key at first boot and grants it to decoys through
enrollment — so decoys need the flag too, or they fall back to the baked compile-time key, never
receive the grant, and stay invisible to the controller even while healthy. To run the simpler
baked-key regime instead, omit `-DSIMULACRA_FLEET_PROVISION=1` from **every** node (all then share
the key in `components/simulacra_radar/radar_key.h`). Changing any `-D…` flag needs a clean build
(`rm -rf build sdkconfig`) so the old define doesn't linger.

## Repository layout

```
main/                    decoy firmware (churn, persistent devices, probes, self-learning, detection, ESP-NOW)
cyd/                     Vigil controller firmware (display, touch, SD librarian, fleet authority)
components/simulacra_radar/  shared code (wire formats, learning, signatures, rendering)
components/tweetnacl/     vendored TweetNaCl (Ed25519 / X25519)
tools/pcap_learn/         replay a BLE capture through the real learn/detect pipeline
tools/decoy_audit/        score how separable the BLE decoys are from a real crowd
tools/probe_audit/        verify Wi-Fi probe frames are archetype-faithful and Law-3 safe
tools/seq_gate/           post-flash check that each fake phone's 802.11 sequence stays independent
docs/                     design specs, implementation plans, hardware notes, and the roadmap
```

## Offline & bench tools

Simulacra's host tools compile the **real firmware code** (not reimplementations), so behaviour is
verified against the same source that runs on-device:

- **`tools/pcap_learn/`** — replay a BLE capture through the actual self-learning pipeline (validate
  structure-only learning, emit a seed library) and the tracker matcher with dwell/co-travel analysis.
- **`tools/decoy_audit/`** — compile the real BLE generator on the host and score how separable the
  synthetic crowd is from a real capture, as a ranked scorecard plus a single regression-gate number.
- **`tools/probe_audit/`** — byte-exact verification that the Wi-Fi probe frames match real-phone
  archetypes and never leak a directed SSID (the Law-3 guard).
- **`tools/seq_gate/`** — a two-board post-flash gate confirming each fake phone keeps its own
  802.11 sequence counter after an IDF/toolchain bump.

Each tool has its own README with build and run steps.

## Recent updates

Newest first. Forward-looking milestones live in [`docs/ROADMAP.md`](docs/ROADMAP.md).

- **Cross-protocol personas (M10 v1).** BLE and Wi-Fi identities are now bound into single,
  co-present synthetic devices that appear and leave together — so a correlator can't isolate your
  real dual-radio phone by filtering out single-radio "ghosts." Persona BLE identities present a
  realistic, Law-3-safe phone shape (terse flags-only / 16-bit service-UUID adverts on rotating
  RPAs), never vendor-matched accessory beacons.
- **Detectability, measured across every axis.** The host audit compiles the real generator *and*
  the real self-learning path, then scores separability from a real capture on address-type mix,
  advertising interval, vendor histogram, AD structure, and presence/lifespan — turning "are the
  decoys convincing?" into a single regression-gate number. Recent passes closed several structural
  tells (AD-structure monoculture, a bogus presence metric) and added a static-infrastructure cohort.
- **Presence & lifespan realism.** Decoys now include persistent static-infrastructure devices and
  per-type address rotation alongside the death/rebirth churn, so the population's come-and-go
  behaviour matches a real crowd instead of a fixed set.
- **Vigil console (CYD).** Live radar/threat dashboard reskinned, plain-language labels, responsive
  touch control, and a per-node fleet roster surfacing TX-health and battery state.
- **Post-flash sequence gate.** A two-board bench check confirms each fake phone keeps an
  independent 802.11 sequence counter after an IDF/toolchain bump.

## Credits

Originally forked from and built on [**0xXyc/splinter**](https://github.com/0xXyc/splinter) — the
project that started the idea. Simulacra extends it into a multi-node, self-learning, fleet-managed
system.

## License

**Currently unlicensed — all rights reserved.** Simulacra is derived from
[0xXyc/splinter](https://github.com/0xXyc/splinter), which is itself unlicensed; those upstream
portions remain under their original author's terms. A permissive license is intended once the
upstream licensing is settled — until then, no redistribution or reuse rights are granted.
