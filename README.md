# Simulacra

> *Raise a host that was never born.* Simulacra conjures a churning crowd of phantom
> Bluetooth devices — copies with no originals — so a watcher cataloguing the room
> loses your real signal among the dead.

*A fork of [Splinter](https://github.com/0xXyc/splinter) by 0xXyc (Jacob Swiz), used with permission. Splinter is the foundation this builds on.*

<img width="1625" height="1077" alt="image" src="https://github.com/user-attachments/assets/b2bd80d8-875c-45b5-8e96-a5309474d132" />

---

A BLE **privacy / anti-tracking decoy** for the ESP32. It continuously fabricates a
churning crowd of plausible-but-fake Bluetooth LE devices so that, in a space you
control, a tracking or scanning system sees lots of ordinary-looking traffic and your
real device(s) don't stand out.

## What it does

Simulacra maintains a **synthetic population** of plausible-but-fake BLE devices that
persists and turns over like a real crowd — rather than flickering a brand-new random
decoy every cycle. On the ESP32-C6 it runs up to **4 genuinely-concurrent** advertisers
(BT 5 extended advertising) and time-slices a larger active set across them.

- a pre-generated **roster** of persistent identities, each with a stable
  **random-static MAC** (what modern phones, watches and earbuds already use for privacy)
  and a frozen, format-correct payload drawn from an **archetype library**
  (`main/templates.c`) — vendor earbuds/fitness/sensor manufacturer-data, iBeacon,
  Eddystone-UID/-URL, and Tile-style service data — each a coherent vendor+interval+payload bundle;
- a **churn engine** that keeps ~`CHURN_ACTIVE_SET` identities "present" at once: each
  dwells for a few minutes, then retires into a cooldown of tens of minutes before it may
  **reappear with the same MAC** — so a scanner sees devices arrive, linger, leave, and
  come back later, exactly like a real space;
- the active set is **time-sliced** across the 4 hardware advertising instances, so the
  crowd is larger than the radio count.

A scanner watching over minutes therefore logs a stable handful of vendor-attributed
devices with staggered arrivals/departures and gradual turnover — not one-shot flicker.

## What it deliberately does NOT do (non-intrusive BLE connections)

Advertising is **non-connectable**, and the payload never carries the Apple subtypes that
make bystanders' phones pop up: Continuity proximity-pairing (`0x07`), nearby-action
(`0x0F`), or Find My (`0x12`). Plain **iBeacon** (Apple company `0x004C`, subtype `0x02`)
*is* emitted — it is a silent location beacon that never triggers a pairing dialog.
Microsoft Swift Pair (`0x0006`) and Google Fast Pair (`0xFE2C`) are likewise never emitted.
A decoy needs realistic *presence*, not pop-up spam aimed at people nearby.

This "refined Law 3" is enforced in two places: the iBeacon encoder hardcodes the
`4C 00 02 15` prefix so it can never drift into a pop-up subtype, and the on-target
self-test scans every roster payload for the forbidden subtypes.

This helps prevent annoying pop-ups that are seen in other "spammers" in other products and firmware variants. This is how we get around that. 

> Intended for privacy/anti-tracking use in a space you control. Don't point it at other
> people's devices.

## Hardware

- **Seeed XIAO ESP32-C6** (v2 target) — BT 5 LE extended advertising, native USB-Serial-JTAG
- **Adafruit ESP32-S3** (QT Py / Feather) — v2 portable target (dual-core; battery-ready on the Feather). Build/BOM guide: [docs/hardware/portable-feather-s3-build.md](docs/hardware/portable-feather-s3-build.md)
- ESP32-WROVER-E (classic dual-core ESP32 with PSRAM) — original v1 single-advertiser target

## Build & flash

Requires ESP-IDF v5.4 (installed at `~/esp/esp-idf`).

```bash
. ~/esp/esp-idf/export.sh          # load the IDF environment into this shell
cd ~/Projects/simulacra
idf.py set-target esp32c6          # one-time; picks up sdkconfig.defaults.esp32c6 (ext-adv)
# or, for an ESP32-S3 board (QT Py / Feather):  idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Flashing needs serial access — your user must be in the `dialout` group
(`sudo usermod -aG dialout "$USER"`, then open a fresh terminal or run `newgrp dialout`).

## Configuration

The synthetic-population tunables live in `main/churn.h`:

| Macro | Default | Effect |
|-------|---------|--------|
| `CHURN_ACTIVE_SET` | 8 | How many identities are "present" at once — the crowd size / density |
| `CHURN_TICK_MS` | 250 | Scheduler tick; each tick advances the state machine and yields |
| `CHURN_DWELL_MIN_MS` / `CHURN_DWELL_MAX_MS` | 3 min / 10 min | How long an identity stays on air before retiring |
| `CHURN_COOLDOWN_MIN_MS` / `CHURN_COOLDOWN_MAX_MS` | 30 min / 60 min | How long a retired identity waits before it may reappear |

Identity-shape tunables live in `main/roster.h`:

| Macro | Default | Effect |
|-------|---------|--------|
| `CHURN_ROSTER_SIZE` (`roster.h`) | 256 | Size of the persistent identity pool |

**Archetype library.** Each identity's payload is built from one of the archetype bundles in
`main/templates.c` — vendor earbuds/fitness/sensor manufacturer-data, iBeacon,
Eddystone-UID/-URL, and Tile-style service data. Each bundle pins a vendor/format together
with its own interval band and (optional) device name, so a decoy can never present an
impossible vendor+payload+timing combination. Tune the crowd by editing the table: the
`weight` column sets each archetype's share of the mix, and the `itvl_min_ms` / `itvl_max_ms`
columns set its advertising cadence. Keep names ≤ 12 chars to stay within the 31-byte budget.

### Observe mode (M5)

Build with `SIMULACRA_OBSERVE=1` (in `main/simulacra_main.c`) to **profile the ambient BLE
environment** instead of advertising. Observe mode passively scans (extended discovery, since
ext-adv is enabled) and aggregates what it hears into a small model of distributions — vendor mix,
per-vendor interval histograms, RSSI spread, advertising-PDU types, distinct-device population, and
arrival rate — persisted to NVS (`main/rf_model.c`). It **stores no per-device identifiers**: every
MAC is hashed with a per-boot salt for in-window dedup only and dropped at capture; the ephemeral
table is wiped each sweep. The model is what M6 will sample to generate a population that matches the
room. The default build (`SIMULACRA_OBSERVE=0`) is unchanged — the decoy. Cadence knobs live at the
top of `main/observe.c` (`OBS_SWEEP_MS`, `OBS_PERSIST_EVERY`; raise from the 15 s test value for a
long-running deployment). This needs the observer role — `CONFIG_BT_NIMBLE_ROLE_OBSERVER=y` in
`sdkconfig.defaults.esp32c6`.

### Antenna (XIAO ESP32-C6)

The firmware drives the XIAO C6 RF switch at boot: GPIO3 low enables the switch, GPIO14 selects the
antenna. It defaults to the **external U.FL antenna** (`SIMULACRA_EXT_ANTENNA 1`). If you run on the
**onboard** ceramic antenna, set `SIMULACRA_EXT_ANTENNA 0` — selecting external with no antenna
attached degrades RF (and noticeably hurts scan/RX sensitivity in observe mode).

## Troubleshooting

- **`apt` says "Release file ... is not valid yet"** — the system clock is wrong. Fix with
  `sudo date -s "$(curl -sI https://www.google.com | grep -i '^date:' | cut -d' ' -f2-)"`
  then `sudo timedatectl set-ntp true`.
- **`fatal error: nimble/nimble_port.h: No such file`** — `main/CMakeLists.txt` needs
  `REQUIRES bt nvs_flash` (already set here).
- **`Permission denied: '/dev/ttyUSB0'`** — you're not in `dialout` yet, or the terminal
  predates the group change; open a new terminal / `newgrp dialout`.
