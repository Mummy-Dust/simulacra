# M7 — Synthetic Wi-Fi Probe-Request Injection (design spec)

**Status:** Approved (2026-07-01). Adds the Wi-Fi dimension to Simulacra. Board-agnostic core;
first hardware-verified on the C5 (Ward), which is dual-band.

**Milestone goal (handoff):** craft fake **probe-request** frames with randomized source MACs and
inject them (`esp_wifi_80211_tx`), non-targeted and randomized, mirroring normal phone scanning — so
a Wi-Fi sniffer in a space you control sees plausible phone-like probe traffic that corresponds to no
real device.

## Scope (decided)

- **Synthetic injection first.** Emit plausible probes from a built-in model (randomized MACs,
  broadcast probes, realistic rates) — Wi-Fi observation→modeling is a *later* milestone (the M5/M6
  equivalent). This mirrors BLE's static-templates → observe→generate arc.
- **Wi-Fi-only build mode.** A separate mode (`SIMULACRA_PROBE`) with BLE idle. BLE + Wi-Fi
  coexistence is **M8**.
- **Dual-band:** 2.4 GHz on all boards; **5 GHz additionally on the C5 (Ward)**. The C6 (Shade) is
  2.4 GHz only.

## Invariants ("Law 3 for Wi-Fi" + others)

- **Broadcast probes only — never directed.** Emit only wildcard-SSID (broadcast) probe requests;
  **never** name specific SSIDs. Directed probes leak a fake preferred-network-list — itself a
  fingerprint / tracking vector, the exact thing we defend against. Also the modern phone default, so
  it's safer *and* more believable. Hardcoded + self-test-guarded.
- **Randomized, locally-administered MACs.** Source MAC has the locally-administered bit set (bit 1
  of octet 0) and is unicast (bit 0 clear) — exactly what privacy-randomizing phones emit. Never a
  real OUI; never a copied/observed MAC (we don't observe Wi-Fi in M7).
- **Non-targeted / non-associating.** Probe requests only — never associate, never send data, never
  target a specific AP or client.
- **No identifiers stored.** Generation is synthetic; nothing is captured or persisted.

## The probe-request frame

A valid 802.11 management frame, type 0 / subtype 4:
- **MAC header:** frame control `0x0040` (probe req), duration 0, **DA = `FF:FF:FF:FF:FF:FF`**,
  **SA = our randomized MAC**, **BSSID = `FF:FF:FF:FF:FF:FF`**, sequence control.
- **Frame body (information elements):** SSID IE (`0x00`, **length 0 = wildcard**), Supported Rates
  IE, Extended Supported Rates IE, DS Parameter Set (current channel). Optionally a generic HT
  capabilities IE for modern realism. The IE set is plausible and generic — a malformed frame is a
  tell.

## Randomized MAC scheme

`probe_random_mac(out[6])`: 6 random bytes, then `out[0] = (out[0] & 0xFC) | 0x02` (set
locally-administered, clear multicast). Regenerate the degenerate all-zero/all-FF cases. This is the
Wi-Fi analog of the BLE random-static address.

## Rotation, cadence & persona

- A small **active set of fake "phones,"** each holding a randomized MAC, **rotated over time**
  (a MAC retires and a fresh one appears) so a sniffer sees phones arrive and leave — the Wi-Fi echo
  of BLE churn, but lightweight and self-contained (not the BLE churn engine).
- **Cadence:** each fake phone emits a short probe burst (a few frames across channels) every few
  seconds; channel-hop the common set. Not a flood.
- **Channels:** 2.4 GHz {1, 6, 11}; **C5 adds 5 GHz** {e.g. 36, 40, 44, 48, 149, 153, 157, 161}.
- **Persona-tuned** (`#if CONFIG_IDF_TARGET_ESP32C5`):
  - **Ward** (C5): ~6–10 fake phones, 2.4 + 5 GHz, denser.
  - **Shade** (C6): ~3–4 fake phones, 2.4 GHz only, faster MAC rotation (the anti-entourage concern
    applies to Wi-Fi too — co-traveling probe MACs are a tracking tell).

## Wi-Fi init & injection

- Init Wi-Fi for raw TX: `esp_netif_init` / `esp_event_loop` (as needed), `esp_wifi_init`,
  `esp_wifi_set_storage(WIFI_STORAGE_RAM)`, `esp_wifi_set_mode(WIFI_MODE_STA)`, `esp_wifi_start`.
- Per burst: `esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE)` (5 GHz channel handling on the C5 to
  be confirmed in the plan), then `esp_wifi_80211_tx(WIFI_IF_STA, frame, len, true)`. Log the rc.
- NVS is already initialized in `app_main`. Confirm any sdkconfig needed for raw mgmt-frame TX in the
  plan (ESP-IDF allows `esp_wifi_80211_tx` for probe requests).

## Verification

- **On-target self-test** (pure frame logic, no radio): build a probe for a fixed MAC and assert:
  frame-control = probe-req; DA and BSSID are broadcast; SA equals the supplied MAC; the **SSID IE is
  present with length 0 (wildcard)**; a Supported-Rates IE is present; and `probe_random_mac` returns
  a **locally-administered, unicast** address that varies across calls. A guard asserts **no non-zero
  SSID IE is ever emitted** (Wi-Fi Law 3).
- **Hardware acceptance (C5):** flash `SIMULACRA_PROBE`; the injector logs each `esp_wifi_80211_tx`
  rc over serial, so injection liveness is confirmable from the dev side. The **visual** acceptance —
  randomized-MAC broadcast probes at a plausible rate across channels — needs a **Wi-Fi sniffer on
  the user side** (Wireshark monitor mode / Kismet / a phone probe-sniffer), since passive Wi-Fi
  sniffing isn't available from the agent. On the C5, confirm probes appear on **both** 2.4 and 5 GHz.

## Out of scope (M7)

Wi-Fi observation/modeling (later milestone); BLE + Wi-Fi coexistence (M8); directed / SSID-list
probes (deliberately never); deep 5 GHz regulatory/rate tuning beyond basic channel hopping.

## Honest note

This is slightly more "active" than BLE advertising — transmitting 802.11 management frames rather
than only beaconing. Same threat-model framing: a space you control, non-targeted, randomized, and
mirroring what every phone already does constantly. Keep it to broadcast probes at realistic rates.
