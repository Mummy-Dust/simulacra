# Roadmap

> **Project:** **Simulacra** — a fork of [Splinter](https://github.com/0xXyc/splinter) by 0xXyc (Jacob Swiz), used with permission.
>
> A BLE/RF **anti-tracking decoy**: generate a synthetic population of plausible-but-fake
> wireless devices that statistically matches the local RF environment but corresponds to
> **no real device**, so your real devices stop standing out in the crowd.

Each milestone must leave a **flashable, working device** — we extend the horizon, we don't
break what works.

## Design laws (every milestone respects these)

1. **Aggregates only** — store distributions/templates, never per-device identifiers. Hash or
   drop real MACs, names, and payloads at capture time.
2. **No verbatim replay** — synthesize new identities; never re-broadcast a captured real one.
3. **Non-connectable only** — broadcast-only; never the Apple/Microsoft/Google pairing-popup
   payload formats.
4. **Population-match** — the active fakes never far exceed the observed real device density.
5. **No raw capture in shipping builds** — raw dumps live behind a compile-time debug flag.

---

## Phase 1 — Foundation ✅ (M3 — done, hardware-verified on ESP32-C6)

- Identity pool (256 synthetic identities) + churn engine: an active set of ~8, each on-air for
  minutes (dwell), then a 30–60 min cooldown, then **reappears with the same MAC**.
- 4 concurrent extended-advertising instances; the active set is time-sliced across them.
- Vendor templates (company ID + optional name) with tunable probabilities.
- On-target self-test asserting the invariants (e.g. no identity reappears before cooldown).

## Phase 2 — Realism (M4–M6)

- **M4 — Per-vendor payload templates.** Structurally-valid payloads (iBeacon/Tile/fitness-band
  shaped); vendor + interval + payload sampled **jointly** so no impossible combinations occur.
- **M5 — Observe → model.** Passive BLE scan + Wi-Fi promiscuous capture; extract **features
  only**; hash-and-drop identifiers immediately; aggregate into a distribution model of the
  local environment. (Data-discipline laws bite hardest here.)
- **M6 — Generate from model + population-match.** Roster drawn from the live model (vendor mix,
  intervals, payloads); active-set size driven by observed device density; re-profile on cadence.

## Phase 3 — Multi-signal & power (M7–M8)

- **M7 — Wi-Fi probe injection.** Randomized-MAC probe-request frames mimicking real phone scanning.
- **M8 — Coexistence & duty-cycle.** BLE advertise + BLE/Wi-Fi scan + Wi-Fi probe share the one
  2.4 GHz radio without starving; battery-aware (e.g. don't re-profile while stationary); runs for
  days on a LiPo.

## Phase 4 — Hardening / anti-correlation (M9+, the moonshot)

*Aimed at fingerprinting + multi-modal correlation systems (e.g. ALPR-linked RF harvesting such as
Leonardo ELSAG SignalTrace). Long-horizon and exploratory — but each step still ships a working
device. These specifically target the two layers a simple decoy can't beat: **RF fingerprinting**
and **cross-modal correlation**.*

- **M9 — The Coven (multi-node mesh).** Several coordinated nodes with **heterogeneous hardware**
  (C6 / C5 / S3 / nRF) so the decoys carry genuinely distinct hardware RF fingerprints *and*
  spatial / independent-motion diversity a single emitter physically cannot fake. The single
  biggest hardening — one radio can't forge N fingerprints; many radios can.
- **M10 — Cross-protocol personas.** Bind a BLE identity + a Wi-Fi identity (later sub-GHz) into
  one synthetic "device" that emits consistently across protocols and appears/leaves together —
  defeating correlators that filter BLE-only ghosts. **Same-board v1 done 2026-07-16
  (`feat/cross-protocol-personas`, firmware compile-verified esp32c5+esp32c6); mesh-distributed
  personas — splitting a persona's two radios across physically separated nodes to beat the
  co-location tell — remain future.**
- **M11 — Targeted mimicry (mimic ring).** Detect your own device(s) and generate decoys that
  clone their vendor/type, so your real device is one of many identical signatures instead of a
  unique one co-occurring with you.
- **M12 — Sub-GHz / TPMS dimension.** A CC1101 add-on to flood decoy tire-pressure-sensor IDs —
  attacking a per-vehicle anchor that BLE/Wi-Fi noise can't touch. (Sub-GHz TX has its own
  regulatory weight; keep it low-power and deliberate.)
- **M13 — Counter-surveillance scry.** Turn the sensing inward-out: detect active RF collection
  (probe/scan-request emitters, collector signatures) and alert / adapt duty-cycle.

## Honest ceilings (design around these — never overpromise)

- **Additive only.** It can't silence your real device. The strongest posture is Simulacra **+
  emission hygiene** (radios off / airplane mode / Faraday in transit; devices with good MAC *and*
  timing randomization; ditch always-on wearables).
- **Can't touch non-RF / legal anchors** — a license plate, cellular identity.
- **One radio can't forge N hardware fingerprints** — which is exactly why the Coven matters.
- **Decoys, not jamming.** Jamming and cellular spoofing are illegal and a different (worse) project.
- **Realistic claim, even maxed out:** *raises the cost of and degrades automated / mass RF
  correlation — especially as a heterogeneous multi-node mesh — not a guarantee against a targeted,
  fingerprinting-grade adversary.*

## Hardware status

- **Working node:** Seeed XIAO ESP32-C6. Firmware builds for **C6** (✅ verified), **S3** (builds;
  ext-adv unreliable on its older BLE controller), **C5** (builds with `--preview`; HW test pending).
- **Inbound:** SparkFun Thing Plus C6 (battery-ready node, onboard charging + fuel gauge),
  ESP32-C5-WROOM-1 (dual-band Wi-Fi 6 + 8 MB PSRAM — the moonshot chip if its ext-adv works).
