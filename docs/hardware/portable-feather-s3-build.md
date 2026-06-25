# Portable Splinter — Feather ESP32-S3 build

A battery-powered, place-and-go splinter node. The radio work (BLE synthetic
population now; Wi-Fi capture/inject later) all runs on the S3's single 2.4 GHz
radio — **no SDR or extra RF hardware**. This build is **solder-free**: every
peripheral is STEMMA QT (I²C) or JST.

> Develop on the **QT Py S3** you already have (USB-powered). Move to the **Feather
> S3** for a deployable battery node — it adds LiPo charging + a fuel gauge. The
> firmware is identical (`idf.py set-target esp32s3`); only the board changes.

## Bill of materials

### Core — minimum standalone

| Item | Purpose | Adafruit PID | ~$ |
|------|---------|--------------|----|
| ESP32-S3 Feather (4 MB flash / 2 MB PSRAM) | Brains + radio; USB-C, JST LiPo, MAX17048 fuel gauge, charging, STEMMA QT, NeoPixel | 5477 | 17.50 |
| LiPo 3.7 V, JST-PH (2000–2500 mAh) | Untethered power | 2011 / 328 | 9–15 |
| Inline JST-PH battery switch | True off without unplugging the cell | — | 2–3 |
| USB-C cable | Flash + charge | — | 5 |
| Enclosure (project box or 3D-printed Feather+LiPo case) | Place-and-go | — | 5–10 |

### Optional add-ons (M8 + standalone UX) — all solder-free

| Item | Why | Adafruit PID | ~$ |
|------|-----|--------------|----|
| STEMMA QT IMU (LSM6DSOX+LIS3MDL 9-DoF, or LSM6DS3TR-C 6-DoF) | M8 motion gating ("don't re-profile while stationary") | 4517 / 4503 | 6–13 |
| STEMMA QT cable | Connect the IMU | 4210 | 1 |
| STEMMA QT OLED 128×64 | On-device status (active-set size, density, battery %) | 5027 | 12 |
| Adalogger FeatherWing (microSD + RTC) | Optional on-device model export / raw-capture debug (model otherwise lives in NVS) | 2922 | 9 |

Core ≈ **$40–50**. Core + IMU ≈ **$50–65**. Full M8 ≈ **$75–90**.

## What connects to what

```
                 ┌───────────────────────────┐
   USB-C ────────┤ ESP32-S3 Feather (5477)    │
 (flash/charge)  │                            │
                 │  [STEMMA QT] ──────────────┼──▶ IMU (LSM6DSOX)   ┐
                 │  [STEMMA QT] (daisy-chain) ┼──▶ OLED (optional)  ├ shared I²C
                 │  MAX17048 fuel gauge ───────(onboard, I²C 0x36)  ┘
                 │                            │
   LiPo ──[switch]──[JST-PH]── BAT in         │
                 │  NeoPixel (onboard) status │
                 │  PCB antenna (onboard) 2.4G│
                 └───────────────────────────┘
```

- **IMU → Feather:** one STEMMA QT cable. Daisy-chain the OLED to the IMU's second
  STEMMA QT port if you add it. No soldering, no pull-ups (built in).
- **LiPo → Feather:** JST-PH, with the inline switch spliced on the **positive**
  lead (or use a switched JST extension).
- **USB-C:** flashing and LiPo charging share the one port.

### I²C bus map (no address conflicts)

| Device | Addr | Notes |
|--------|------|-------|
| MAX17048 fuel gauge | 0x36 | onboard the Feather (not on the QT Py) |
| LSM6DSOX (accel/gyro) | 0x6A | IMU |
| LIS3MDL (magnetometer) | 0x1C | IMU (9-DoF combo) |
| OLED (SH1107) | 0x3C | optional |

SPI stays free for the Adalogger SD wing. The onboard NeoPixel is on its own GPIO.

## Power & charging

- **Charge:** plug USB-C; the Feather's CHG LED lights while charging (~500 mA default).
- **Fuel gauge:** MAX17048 reports battery % over I²C — used in M8 for power telemetry.
- **Runtime:** S3 with Wi-Fi+BLE active ≈ 80–150 mA average (bursty on TX). A 2000 mAh
  cell ≈ ~15–24 h all-on, far longer once M8 duty-cycling lands.
- **On/off:** inline switch on the battery is cleanest. Alternatively tie a slide
  switch between the Feather's `EN` pin and `GND` (EN low = off).

> ⚠️ **LiPo polarity.** Adafruit cells match the Feather's JST-PH polarity.
> **SparkFun/Amazon cells are frequently reversed** — a backwards cell can destroy
> the board. Use Adafruit cells or verify polarity with a meter before first plug-in.
> Standard LiPo care: don't puncture, don't charge unattended the first time.

## Firmware gotchas for the S3 boards

- **Feather S3 — I²C power enable.** On the ESP32-S3 Feather the STEMMA QT bus *and*
  the onboard MAX17048 are behind a power-enable GPIO (`PIN_I2C_POWER`, ~GPIO7).
  Firmware must drive it **HIGH at boot before I²C init**, or the IMU/fuel gauge
  won't ACK. (The **QT Py S3 has no such gate** — its STEMMA QT port is always
  powered, but it also has no fuel gauge/battery circuit.) Confirm the exact pin
  against your board revision's pinout.
- **Native USB-JTAG.** Both boards flash over USB-C with no external adapter. If a
  flash won't start: hold **BOOT**, tap **RESET**, release **BOOT** to force the
  bootloader. The port re-enumerates on reset (same behavior as the C6).
- **Scanning is off by default.** The shared `sdkconfig.defaults` keeps the NimBLE
  OBSERVER role disabled (M3 is broadcast-only). M5 (observe → model) will need
  `CONFIG_BT_NIMBLE_ROLE_OBSERVER=y` plus Wi-Fi promiscuous setup.

## Assembly (solder-free)

1. Plug the IMU into the Feather's STEMMA QT port with the cable. (Daisy-chain the
   OLED to the IMU if used.)
2. Splice the inline switch onto the LiPo's positive lead; plug the LiPo into the
   Feather's JST-PH.
3. Seat everything in the enclosure; route USB-C to an accessible edge.
4. Flash over USB-C (below), then run on battery.

## Flashing

The repo already carries `sdkconfig.defaults.esp32s3` (ext-adv enabled), so:

```powershell
# IDF env (Python 3.12 ahead of 3.14), then:
idf.py set-target esp32s3      # one-time; picks up sdkconfig.defaults.esp32s3
idf.py build
idf.py -p COM<NN> flash        # find the new COM port after plugging the board in
```

PSRAM is left off in the defaults so the image boots on both QT Py S3 variants. If
you confirm a 2 MB-PSRAM board and want the M5/M6 buffering headroom, add
`CONFIG_SPIRAM=y` to `sdkconfig.defaults.esp32s3` and rebuild.

## Scaling note

The ceiling on "how many fake devices" is **radio airtime, not CPU**. One node
time-slicing 4 ext-adv instances sustains a few dozen convincing identities plus
probe injection. For a denser/larger deployment, run **several cheap nodes** spread
around the space rather than one bigger chip — more airtime *and* realistic spatial
RSSI diversity a single point source can't fake.
