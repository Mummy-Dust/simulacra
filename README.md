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

- Rotating decoy crowd across BLE (and Wi-Fi probe) with realistic MACs, vendor shapes, and
  advertising intervals.
- On-device **self-learning** of ambient device *shapes* into new decoy archetypes (structure-only,
  Law-3 gated), synced across the fleet and persisted to an **encrypted-at-rest** SD library on Vigil.
- **Passive follower detection** and **tracker/surveillance fingerprint** matching.
- **Signed fleet control:** Vigil pushes Ed25519-signed behaviour presets (PAUSE / STEALTH /
  NORMAL / DENSE / MAX) to every decoy over ESP-NOW.
- **On-air fleet enrollment (ECDH):** decoys ship with no shared transport key and enroll on-air via
  a mutually-authenticated 3-message handshake, so **capturing a decoy does not compromise the fleet.**
- **Vigil console:** live radar/threat display, touch-driven controls, and an on-panel roster to
  enroll or revoke fleet members.

## Security model

- **Asymmetric by design.** The controller (Vigil) holds the private signing key; decoys hold only
  the public key. A captured decoy can verify commands but cannot forge them or control the fleet.
- **Never trust the wire.** Every synced/seeded template is re-gated (budget + Law-3 + hash recompute)
  on receipt, so a leaked key or spoofed node still cannot inject a forbidden identity.
- **Structure-only learning.** Real-world captures are stripped to skeletons; no bystander identities
  or names are stored or emitted.

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
main/                    decoy firmware (churn, self-learning, detection, ESP-NOW link)
cyd/                     Vigil controller firmware (display, touch, SD librarian, fleet authority)
components/simulacra_radar/  shared code (wire formats, learning, signatures, rendering)
components/tweetnacl/     vendored TweetNaCl (Ed25519 / X25519)
tools/pcap_learn/         offline host tools — replay a BLE capture through the real pipeline,
                          validate structure-only learning, and scan for trackers
docs/design/              specs and implementation plans
```

## Offline tools

`tools/pcap_learn/` builds the *real* firmware pipeline on a host to replay a BLE capture:
validate that learning is structure-only, emit a seed library, and run the tracker matcher with
dwell/co-travel analysis. See `tools/pcap_learn/README.md`.

## Credits

Originally forked from and built on [**0xXyc/splinter**](https://github.com/0xXyc/splinter) — the
project that started the idea. Simulacra extends it into a multi-node, self-learning, fleet-managed
system.

## License

**Currently unlicensed — all rights reserved.** Simulacra is derived from
[0xXyc/splinter](https://github.com/0xXyc/splinter), which is itself unlicensed; those upstream
portions remain under their original author's terms. A permissive license is intended once the
upstream licensing is settled — until then, no redistribution or reuse rights are granted.
