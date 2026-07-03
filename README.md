```
███████╗██╗███╗   ███╗██╗   ██╗██╗      █████╗  ██████╗██████╗  █████╗
██╔════╝██║████╗ ████║██║   ██║██║     ██╔══██╗██╔════╝██╔══██╗██╔══██╗
███████╗██║██╔████╔██║██║   ██║██║     ███████║██║     ██████╔╝███████║
╚════██║██║██║╚██╔╝██║██║   ██║██║     ██╔══██║██║     ██╔══██╗██╔══██║
███████║██║██║ ╚═╝ ██║╚██████╔╝███████╗██║  ██║╚██████╗██║  ██║██║  ██║
╚══════╝╚═╝╚═╝     ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═╝ ╚═════╝╚═╝  ╚═╝╚═╝  ╚═╝
```

> *Raise a host that was never born.* Simulacra conjures a churning crowd of phantom
> Bluetooth and Wi-Fi devices — copies with no originals — so a watcher cataloguing the
> room loses your real signal among the dead.

*A fork of [Splinter](https://github.com/0xXyc/splinter) by 0xXyc (Jacob Swiz), used with permission — the foundation this builds on.*

---

**Simulacra is a modular, multi-node counter-surveillance system for the ESP32.** In a space
you control, a fleet of small cooperating nodes floods the air with a *synthetic crowd* of
plausible-but-fake BLE and Wi-Fi devices, watches for anything that persistently follows you,
and surfaces it all on a separate radar screen — the nodes talking to each other over an
encrypted link. No cloud, no app, no accounts. Just radios that lie convincingly.

It isn't a jammer and it isn't a spammer. It builds *presence*: a believable, drifting
population that a tracker has to swim through, while your real device sits quietly in the noise.

## The system: cooperating nodes

Simulacra is built as **roles, not one monolithic gadget**. A board's job is chosen at build
time, and boards work *together* — Ward or Shade fills the room, Vigil watches from your pocket,
a sniffer audits the link. They coordinate over **ESP-NOW**, encrypted end-to-end with
**AES-256-GCM** under a shared pre-shared key.

### Decoy node — the crowd

The heart of the system. It fabricates and sustains a synthetic population of BLE + Wi-Fi
devices (see [Inside the decoy](#inside-the-decoy)). It ships in two named build profiles, tuned
to how and where it runs:

- **Ward** — *the fixed guardian.* Built for a room or a vehicle: mains-powered, stationary,
  and dense. Runs on the **ESP32-C5**, so it works **both 2.4 GHz and 5 GHz**, injects Wi-Fi
  bursts aggressively (~2 s cadence with periodic 5 GHz excursions), and carries a heavier
  crowd (denser active set). Because it doesn't move, its anti-drift churn is disabled — a Ward
  holds its ground.
- **Shade** — *the ghost that walks with you.* Built for EDC / on-body carry: lean,
  battery-friendly, **2.4 GHz only**, on the **ESP32-C6**. Thinner Wi-Fi cadence, a smaller
  crowd, and **anti-entourage churn**: when it detects the RF environment change sharply
  (you've moved rooms, or someone is trailing you), it accelerates turnover ~3× for a couple of
  minutes so the decoy crowd reshuffles as fast as the real world around it.

Both profiles are the *same firmware*; the persona is selected automatically from the chip
target (`#if CONFIG_IDF_TARGET_ESP32C5` → Ward, otherwise Shade).

### Vigil — the watch

**Vigil** is a physically separate **Cheap Yellow Display (CYD)** that renders a live **threat radar** —
rings, sweep, follower dots placed by signal strength, and decoy/population stats — with a
touch to page between views. It is **receiver-only** and holds no secrets of its own beyond the
shared key: it *asks* a decoy for a status snapshot on demand, and the decoy answers with a
short burst of **encrypted, hash-only** telemetry over ESP-NOW. The decoy stays **silent until
asked**, so the link adds no ambient chatter. Glance at the screen; the decoy stays hidden.

### Sniffer node — the audit

A spare board flashed as a **channel-1 promiscuous sniffer** that decodes the link *without the
key* — used to verify the system's own opsec: that the decoy is silent until requested, that
telemetry on the air is ciphertext (never plaintext status), and that the decoy's source MAC is
locally-administered, not its factory address. There's also a Wi-Fi-side probe sniffer for
profiling the ambient 802.11 environment.

> **How they cooperate.** Broadcast ESP-NOW on a fixed channel, AES-256-GCM with a per-frame
> nonce and replay rejection, hash-only payloads (see [Design laws](#design-laws)). Roles are
> compile-time gates (`SIMULACRA_ESPNOW`, `SIMULACRA_ESPNOW_SNIFF`, `SIMULACRA_SNIFF`,
> `SIMULACRA_OBSERVE`, …) so one codebase flashes into whichever node you need.

## Inside the decoy

The decoy maintains a **synthetic population** that persists and turns over like a real crowd —
not a brand-new random device flickering every cycle.

- **Persistent roster.** A pool of stable identities (`main/roster.c`), each with a fixed
  **random-static MAC** — exactly the privacy address type modern phones, watches and earbuds
  already use — and a frozen, format-correct payload drawn from an **archetype library**
  (`main/templates.c`): vendor earbuds / fitness / sensor manufacturer-data, iBeacon,
  Eddystone-UID/-URL, and Tile-style service data. Each archetype pins a coherent
  vendor + interval + payload bundle, so a decoy can never present an impossible combination.
- **Churn engine.** Roughly `CHURN_ACTIVE_SET` identities are "present" at once. Each **dwells**
  for a few minutes, then retires into a **cooldown** of tens of minutes before it may
  **reappear with the same MAC** — so a scanner sees devices arrive, linger, leave, and return,
  exactly like a real space.
- **Genuinely concurrent radios.** On BT-5 chips the active set is time-sliced across up to
  **4 hardware extended-advertising instances**, so the crowd is larger than the radio count.
- **Wi-Fi dimension.** Alongside BLE, the decoy injects synthetic 802.11 **probe requests**
  (`main/probe.c`) from a small set of fake "phones," each using a randomized,
  locally-administered MAC that rotates over time. Only **broadcast / wildcard-SSID** probes are
  ever sent — never directed probes that would leak a fake preferred-network list.
- **Coexistence + live re-profiling.** BLE and Wi-Fi run **concurrently** via ESP-IDF software
  coexistence (the default build). Periodically the coordinator scans the ambient BLE
  environment for ~15 s, models it, and **reshapes the synthetic crowd** — size, vendor mix,
  intervals — to match the room it's actually in, with no reflash.

A scanner watching over minutes therefore logs a stable handful of vendor-attributed devices
with staggered arrivals, gradual turnover, and matching Wi-Fi noise — indistinguishable from an
ordinary busy room. Your real device is one more face in that crowd.

## Threat Radar

While it decoys, Simulacra also **watches for a device that is following you**. It taps the same
ambient scans the decoy already runs and flags a **stable-identity device seen with you across
multiple distinct RF environments** — a behavioural "this thing moves with me" signal, not a
fingerprint.

- A **location-epoch** advances whenever a re-profile measures a materially changed environment
  — an RF-neighbourhood proxy, not GPS.
- A device seen with meaningful presence across **3 distinct location-epochs** becomes a
  **confirmed follower**.
- Alerts surface three ways: serial (`THREAT confirmed …` with the live MAC so you can locate
  it, plus RSSI-throttled "getting-warmer" updates), an optional board LED, and — the point of
  the system — **Vigil**, the remote radar display described above.

**Honest scope.** This is a *behavioural follower-detector*, not a universal tracker scanner. It
catches **non-rotating** followers (e.g. a fixed-MAC beacon slipped into a bag). Commercial tags
that rotate their addresses — AirTag, SmartTag, Tile — are **not** caught by this layer; that's
known-tracker fingerprinting, a planned detection module. A device you're simply often near may
occasionally flag (an allowlist is planned).

## Design laws

Three principles the whole project is held to:

1. **Be non-intrusive ("Law 3").** Advertising is **non-connectable**, and the payload *never*
   carries the subtypes that make bystanders' phones pop up pairing dialogs — Apple Continuity
   (`0x07`), nearby-action (`0x0F`) or Find My (`0x12`), Microsoft Swift Pair (`0x0006`), Google
   Fast Pair (`0xFE2C`). Plain **iBeacon** *is* emitted (a silent location beacon that triggers
   nothing). This is what separates a decoy from a "spammer": realistic presence, never pop-up
   spam aimed at people nearby. It's enforced twice over — the iBeacon encoder hardcodes a safe
   prefix, and an on-target self-test scans every roster payload for forbidden subtypes.
2. **Keep no more than you must.** Detection candidates live **hashed in RAM** with a per-install
   salt; only *confirmed* threats persist, and the decoy **never flags its own** advertised MACs.
   Telemetry between nodes is hash-only and encrypted — raw identifiers never travel the link.
3. **Aim it at your own space.** Simulacra is for privacy and anti-tracking in an environment you
   control. Don't point it at other people's devices.

## Hardware

Current, supported nodes:

| Node | Board | Role | Notes |
|------|-------|------|-------|
| **Ward** decoy | **ESP32-C5** | dense dual-band decoy | 2.4 + 5 GHz, mains, stationary |
| **Shade** decoy | **ESP32-C6** (e.g. SparkFun Thing Plus C6) | lean mobile decoy | 2.4 GHz, battery/EDC |
| **Vigil** display | **CYD** — ESP32-2432S028 | receiver-only screen | classic ESP32 + ILI9341 + touch |
| **Sniffer** | any spare C5/C6 | opsec / probe audit | verification role |

A decoy needs **BT-5 extended advertising** (`CONFIG_BT_NIMBLE_EXT_ADV`), which the C5/C6 have
and the classic ESP32 does not — which is exactly why the CYD makes a perfect *display* node
(receive-only) but never a decoy.

## Build & flash

Two ESP-IDF projects live in this repo:

- the repo root — the **decoy / sniffer** firmware (targets `esp32c5` or `esp32c6`);
- [`cyd/`](cyd/) — the **radar display** firmware (target `esp32`).

Both build with a standard ESP-IDF v5.x toolchain:

```bash
. ~/esp/esp-idf/export.sh          # load ESP-IDF into this shell
idf.py set-target esp32c6          # esp32c5 (Ward) · esp32c6 (Shade) · esp32 (CYD, from cyd/)
idf.py build
idf.py -p <PORT> flash monitor
```

The default build (all flags `0`) is the **combined coexist decoy** — BLE + Wi-Fi together.
Select a different role at build time:

| Flag | Node / behaviour |
|------|------------------|
| *(none set)* | Combined BLE + Wi-Fi decoy — the default |
| `SIMULACRA_ESPNOW=1` | Decoy that also answers a radar-display node over ESP-NOW |
| `SIMULACRA_ESPNOW_SNIFF=1` | Channel-1 ESP-NOW opsec sniffer |
| `SIMULACRA_SNIFF=1` | Wi-Fi probe sniffer (promiscuous capture) |
| `SIMULACRA_OBSERVE=1` | BLE-only ambient observe + model (never advertises) |
| `SIMULACRA_PROBE=1` | Wi-Fi-only probe injector (bench) |
| `CHURN_SELFTEST=1` | On-target host-logic self-test; radio idle, PASS/FAIL serial |

The combined binary exceeds the default factory partition, so the provided
`sdkconfig.defaults.esp32c{5,6}` set `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` — no manual
Kconfig change needed.

## Configuration

The crowd's shape lives in a few tunables:

| Macro (file) | Default | Effect |
|--------------|---------|--------|
| `CHURN_ACTIVE_SET` (`churn.h`) | 8 | How many identities are "present" at once — crowd density |
| `CHURN_DWELL_MIN/MAX_MS` (`churn.h`) | 3 / 10 min | How long an identity stays on air before retiring |
| `CHURN_COOLDOWN_MIN/MAX_MS` (`churn.h`) | 30 / 60 min | How long a retired identity waits before it may reappear |
| `CHURN_ROSTER_SIZE` (`roster.h`) | 256 | Size of the persistent identity pool |

Tune the *mix* by editing the archetype table in `main/templates.c` — the `weight` column sets
each archetype's share and the interval columns set its cadence (keep device names ≤ 12 chars to
fit the 31-byte budget). Population size, vendor mix and 5 GHz behaviour are **persona-driven**
(Ward vs Shade) and auto-selected from the chip target.

**Web config dashboard.** At boot the decoy can raise an **on-demand open Wi-Fi AP + captive
dashboard** for ~30 s (gate `SIMULACRA_WEBUI`, on by default): join it from a phone to see live
status and toggle detection/churn, clear threats, or reboot — then the AP drops and Wi-Fi is
handed back to the decoy.

## Roadmap & honest limits

- **Known-tracker fingerprinting** — a detection layer for rotating commercial tags (AirTag /
  SmartTag / Tile) that the behavioural follower-detector can't catch.
- **Wi-Fi observe → match** — profile the ambient probe environment and generate synthetic probe
  traffic that matches the room, the way the BLE side already does.
- **OTA updates** — now that a screen exists to confirm/rollback.
- **More cooperating roles** — the modular node model is young; expect additional roles as the
  fleet grows.

## Credits

Built on [Splinter](https://github.com/0xXyc/splinter) by **0xXyc (Jacob Swiz)**, used with
permission. Simulacra extends it into the multi-node system described above.

> Intended for privacy / anti-tracking use in a space you control. Don't point it at other
> people's devices.
