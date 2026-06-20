# splinter
"Let me tell you why you're here. You're here because you know something. What you know you can't explain, but you feel it. You've felt it your entire life, that there's something wrong with the world. You don't know what it is, but it's there, like a splinter in your mind, driving you mad." — Morpheus

<img width="1625" height="1077" alt="image" src="https://github.com/user-attachments/assets/b2bd80d8-875c-45b5-8e96-a5309474d132" />

---

A BLE **privacy / anti-tracking decoy** for the ESP32. It continuously fabricates a
churning crowd of plausible-but-fake Bluetooth LE devices so that, in a space you
control, a tracking or scanning system sees lots of ordinary-looking traffic and your
real device(s) don't stand out.

## What it does

Every `SIMULACRA_ROTATE_MS` (default 250 ms) splinter retires the current advertisement
and mints a new decoy with:

- a fresh **random-static MAC** — exactly what modern phones, watches and earbuds already
  do for privacy, so the churn looks realistic;
- a random vendor drawn from `main/decoy_vendors.h`, surfaced via the **Company ID in
  manufacturer-specific data** (the spec-defined vendor signal a scanner actually reads);
- an optional short device name and a benign random payload.

A scanner sampling over a few seconds therefore logs dozens of distinct, vendor-attributed
devices appearing and disappearing.

## What it deliberately does NOT do (non-intrusive BLE connections)

Advertising is **non-connectable** and the payload is never shaped like
Apple Continuity (`0x004C`), Microsoft Swift Pair (`0x0006`), or Google Fast Pair
(`0xFE2C`). Those formats trigger pairing pop-ups on bystanders' phones/PCs — a decoy
needs realistic *presence*, not pop-up spam aimed at people nearby, so those payloads are
never emitted. See the header comment in `main/decoy_vendors.h`.

This helps prevent annoying pop-ups that are seen in other "spammers" in other products and firmware variants. This is how we get around that. 

> Intended for privacy/anti-tracking use in a space you control. Don't point it at other
> people's devices.

## Hardware

- ESP32-WROVER-E (classic dual-core ESP32 with PSRAM)
- USB serial at `/dev/ttyUSB0`

## Build & flash

Requires ESP-IDF v5.4 (installed at `~/esp/esp-idf`).

```bash
. ~/esp/esp-idf/export.sh          # load the IDF environment into this shell
cd ~/Projects/splinter
idf.py set-target esp32            # one-time, generates sdkconfig from sdkconfig.defaults
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Flashing needs serial access — your user must be in the `dialout` group
(`sudo usermod -aG dialout "$USER"`, then open a fresh terminal or run `newgrp dialout`).

## Configuration

Tunables live at the top of `main/simulacra_main.c`:

| Macro | Default | Effect |
|-------|---------|--------|
| `SIMULACRA_ROTATE_MS` | 250 | How often a brand-new decoy takes the air (lower = denser crowd) |
| `SIMULACRA_NAME_PROB` | 60 | % chance a decoy advertises a device name |
| `SIMULACRA_MFG_PROB` | 85 | % chance a decoy carries vendor manufacturer data |
| `SIMULACRA_ADV_MS` | 120 | On-air advertising interval per decoy (ms) |

Add more vendors/names to `main/decoy_vendors.h` for a denser, more varied crowd (keep
names ≤ 12 chars to stay within the 31-byte advertising budget).

## Troubleshooting

- **`apt` says "Release file ... is not valid yet"** — the system clock is wrong. Fix with
  `sudo date -s "$(curl -sI https://www.google.com | grep -i '^date:' | cut -d' ' -f2-)"`
  then `sudo timedatectl set-ntp true`.
- **`fatal error: nimble/nimble_port.h: No such file`** — `main/CMakeLists.txt` needs
  `REQUIRES bt nvs_flash` (already set here).
- **`Permission denied: '/dev/ttyUSB0'`** — you're not in `dialout` yet, or the terminal
  predates the group change; open a new terminal / `newgrp dialout`.
