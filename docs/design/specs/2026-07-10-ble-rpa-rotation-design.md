# BLE Persistent Devices + Per-Type Address Rotation — Design

**Date:** 2026-07-10
**Status:** Approved (design) — implementation plan to follow
**Builds on:** the existing BLE decoy stack (`churn.c` / `roster.c` / `templates.c` / `generate.c`)
and its host self-audit (`tools/decoy_audit`, which already closed the static/RPA/NRPA *snapshot* tell).
**Milestone scope:** BLE PAN camouflage **Milestone A** — individual persistent devices and their
address-rotation behavior. **Milestone B (persona clusters + ecosystem profiles)** is a separate
follow-on design that builds directly on this one.

## 1. Problem

The BLE decoy crowd already looks right in a **single snapshot**: `roster.c` presents a realistic
blend of address subtypes (~52% static-random / 36% resolvable-private (RPA) / 12% non-resolvable
(NRPA)), `generate.c` matches the real interval and vendor distributions, and `decoy_audit` scores
all three snapshot tells at ~0.001 Jensen–Shannon divergence against a real capture.

But the crowd is trivially synthetic **over time**. Each decoy is modelled as a fixed identity: it is
born with **one address held for its entire life**, advertises for a 3–10 min dwell, then retires and
cools down. Nothing ever changes its address while it is present. A real modern phone/watch/earbuds
using BLE privacy does the opposite — it is a **persistent device that rotates its address every
~15 minutes** while keeping the same advertising behaviour.

This produces a temporal tell the snapshot audit cannot see:

1. **No address rotation.** A decoy wearing an RPA-subtype address (top-2-bits `01`) that *never
   rotates* is self-contradictory: the RPA subtype exists precisely because such devices rotate. An
   adversary observing for longer than one rotation interval buckets every device into "rotates like
   a real privacy device" vs "holds one address then vanishes" — and every decoy falls in the wrong
   bucket. The decoys wear the costume of the category (their real target) without doing the one
   behaviour that defines it.
2. **Lifetime monoculture.** Every decoy lives on the same 3–10 min dwell. A real environment is a
   mixture of **transients** (someone walking past, seen briefly on one address) and **residents**
   (a phone that stays for a long time and rotates several times). A single lifetime band is itself a
   separable signature, and — worse for the defence — it means **no decoy is persistent**, so the
   operator's genuinely-persistent real phone is the only durable device in view.

## 2. Goal

Replace the fixed-identity decoy with a **persistent-device model** whose address rotation and
lifetime match how real BLE devices actually behave over time, so a SignalTrace-class passive
observer watching across minutes/hours cannot separate the decoys from a real crowd on temporal
behaviour, and — critically — so the operator's persistent real phone is hidden among **persistent
decoys** rather than being the only long-lived device present.

Concretely, after this milestone:
- **Resolvable-private (RPA) devices rotate** their address on a realistic, jittered, independent
  schedule (~15 min), preserving their behaviour across the change.
- **Non-resolvable (NRPA) devices rotate** on a shorter schedule.
- **Static-random devices do not rotate** (correct — that is what "static" means); they simply
  persist for their lifetime.
- Devices split into **~70% transient** (short-lived, typically leave before rotating) and
  **~30% resident** (long-lived, rotate several times), so both persistence and turnover are present.

Device-side hardening (the operator's own randomized MAC, radios off when idle) is assumed already in
place; this is the environmental layer on top of it.

## 3. Honest limitation (named, not a footnote)

This milestone makes decoys temporally indistinguishable from a real crowd on **address-rotation and
lifetime behaviour**. It explicitly does **not** deliver the following, and every artifact derived
from this spec must keep saying so:

- **Physical layer unchanged.** RSSI, angle-of-arrival, and co-location are not addressed. Every
  device on a board shares one fixed position and near-constant signal strength; a multi-sensor or
  AoA-capable receiver still separates "signals from a few fixed points" from "the device that moves
  with the target." TX-power dither blurs signal strength; it cannot manufacture spatial separation.
- **Rotation is realistic, not magic.** Because a rotated device deliberately **keeps its behaviour**
  (interval, payload, company id) across the address change — exactly as a real device does — it
  remains **relinkable across its own rotations** by that behavioural continuity, just like a real
  device. Rotation does not make a decoy unlinkable. A decoy's own unlinkability comes only from its
  **bounded lifetime and death**, after which its addresses never return.
- **Cadence must match reality, so it cannot be cranked for volume.** Rotating faster than real
  devices (~15 min) is a *new* tell ("too-fast rotator = fake"), not extra cover. Crowd volume comes
  from population size and turnover, never from fast rotation.
- **The real phone is still the persistent one.** Resident decoys give it cover in any snapshot and
  defeat casual/automated correlation, but a determined adversary doing longitudinal analysis across
  many days and places can still work toward the one persistent cluster that also does real things.
  This raises cost; it is not invisibility.

## 4. Architecture

Follow the pattern proven by the Wi-Fi agent work (`probe_agents`): a **pure, host-testable core**
that owns all timing/lifecycle/rotation decisions, plus thin live glue that talks to NimBLE. Reuse
the existing **content** and **apply** layers unchanged.

- **`main/ble_devices.{c,h}` (new, pure).** Owns the persistent-device population: each device's
  current address, its address subtype, its role (transient/resident), its bounded lifetime, and its
  rotation schedule. Contains **no NimBLE/ESP-IDF calls** — RNG and clock are injected — so it
  compiles and runs on the host under `tools/decoy_audit`. It answers, each tick: *which devices are
  live, what address does each currently present, which ones just rotated, and which just died/were
  born.*
- **Content source — `templates.c` / `generate.c` / `roster.c` (reused).** These already produce a
  device's **frozen behaviour** (payload bytes, advertising interval, company id, tx dither) matched
  to the learned/real distribution. `ble_devices` draws each device's behaviour from this generator
  at birth and holds it fixed for the device's life. The address subtype selection
  (`make_random_addr_mixed`, the ~52/36/12 blend) moves *into the device model* so subtype drives
  rotation policy; the generators keep producing everything else.
- **Presenter — `churn.c` (evolved).** Its per-identity **dwell/cooldown lifecycle is superseded** by
  `ble_devices`' role-based lifetime. Its remaining job is what it already does well: mapping the
  live device population onto the `CHURN_HW_INSTANCES` (4) hardware advertising slots and time-slicing
  when there are more live devices than slots. It must re-apply an instance when its occupant's
  **address changes** (a rotation), not only when the occupant device changes.
- **Apply — `churn_adv.c` (unchanged).** Already stops, reconfigures interval, sets the random
  address, sets the AD payload, and starts an ext-adv instance. A rotation is just a re-apply with a
  new address on the same instance.

### Radio-arbitration constraint

`coexist.c` continues to own radio/BLE arbitration (on the C5, a 5 GHz Wi-Fi excursion silences BLE;
the Wi-Fi probe injector shares the same timeline). `ble_devices` is a **scheduler of intent** — it
decides what the population *should* look like at time `now` — and the presenter applies that intent
only when BLE has the radio. Rotation and birth/death are evaluated against wall-clock `now_ms`, so a
device that should have rotated while BLE was parked simply presents its rotated address on the next
apply; no rotation is "queued up" into a visible burst.

## 5. The persistent-device model

```
typedef enum { BLE_ATYPE_STATIC, BLE_ATYPE_RPA, BLE_ATYPE_NRPA } ble_atype_t;
typedef enum { BLE_ROLE_TRANSIENT, BLE_ROLE_RESIDENT } ble_role_t;

typedef struct {
    uint8_t     addr[6];         // current random address; MSB top-2-bits encode atype
    ble_atype_t atype;           // fixed for life; selects rotation policy
    ble_role_t  role;            // fixed for life; selects lifetime band
    // --- frozen behaviour, drawn from the content generator at birth ---
    uint8_t     payload[31];
    uint8_t     payload_len;
    uint16_t    adv_itvl_ms;
    uint16_t    company_id;      // for inspection/audit
    int8_t      tx_power;
    uint8_t     archetype_idx;
    // --- lifecycle + rotation ---
    uint32_t    born_ms;
    uint32_t    life_ms;         // bounded lifetime; on expiry the device dies
    uint32_t    next_rotate_ms;  // next address rotation (unused for STATIC)
    bool        alive;
} ble_device_t;
```

**Per tick (pure core, given `now_ms`):**
1. **Death / rebirth.** Any device past `born_ms + life_ms` **dies** — it goes silent and its
   addresses never return. The slot is **reborn** as a completely fresh device: new behaviour drawn
   from the generator, new subtype from the blend, new role, new lifetime, new random address, new
   rotation phase. Population turns over continuously; nothing recurs.
2. **Rotation.** For each live device with a rotating subtype (RPA/NRPA) whose
   `next_rotate_ms <= now`: regenerate `addr` as a fresh random address **of the same subtype**
   (`make_random_addr(addr, top2_for_atype)`), keep **all behaviour fields unchanged**, and schedule
   `next_rotate_ms = now + rotate_base(atype) + jitter`. Static devices never rotate. The device
   reports "rotated" so the presenter re-applies it on its instance.
3. **Report.** Expose the live set (address + behaviour per device), plus which devices rotated and
   which were (re)born this tick, for the presenter and for the host dumper.

**Timing bands (initial values; tuned against a real capture during implementation):**

| Parameter | Value | Rationale |
|---|---|---|
| Role split | ~70% transient / ~30% resident | user-chosen; busy churn + persistent cover |
| Transient lifetime | 2–12 min | "walked past"; usually leaves before rotating |
| Resident lifetime | 30–90 min | long enough to rotate several times; persistent cover |
| RPA rotation interval | ~15 min (10–20 min, wide jitter, independent phase) | matches real privacy devices; **not** faster |
| NRPA rotation interval | ~1–10 min | NRPA devices rotate more frequently than RPA |
| Static rotation | none | static-random addresses do not rotate by definition |

A transient RPA device typically dies before its first rotation — which is realistic (a privacy
phone glimpsed briefly on one address). Rotation therefore fires mostly on **resident** RPA/NRPA
devices, which is exactly the cohort meant to demonstrate the behaviour.

**Perceived density (why this adds "layers" honestly).** Concurrent advertising is still bounded by
the 4 hardware instances and by airtime, so this milestone does not raise the *instantaneous* device
count. But an adversary accumulates **every distinct address heard over its dwell**: a resident that
rotates 3–5 times contributes several addresses, and the 70% transient churn continuously introduces
fresh ones. So the **count of distinct identities observed across a window rises substantially**
without any new radio, and the crowd reads as busier and more organic. Raising the *concurrent*
count (more instances / cluster multiplexing) is Milestone B and a tuning task, not this spec.

## 6. Rotation realism (the point, stated precisely)

Owning rotation must reproduce *real* behaviour, not merely "change the address sometimes":

- **Behaviour is preserved across a rotation.** Interval, payload, company id, and tx stay identical;
  only the address changes. This is what makes a rotation read as *the same device* getting a new
  private address. Re-randomizing behaviour on rotation would look like a stream of one-off strangers
  and would be filtered as junk — the opposite of camouflage.
- **Rotations are independent and jittered.** Each device draws its own phase and interval, so no two
  devices rotate on the same boundary. A synchronized rotation volley (many addresses appearing in
  the same instant as many others vanish) is itself a fingerprint of a single coordinating emitter
  and must never occur.
- **Subtype drives policy.** The subtype the device advertises (via its address MSB) and the rotation
  policy it follows must agree: an RPA-subtype device rotates on the RPA schedule, a static device
  never rotates. A mismatch (e.g. a static address that rotates, or an RPA that never does) is the
  exact tell this milestone closes.

There is **no BLE analog of the Wi-Fi sequence-number gate** (BLE advertising PDUs carry no
cross-address monotonic counter that the controller stamps) — but there **is** a NimBLE hardware gate,
discovered on-air during implementation and just as decisive:

> **RPA addresses cannot be set through the normal host API.** `ble_gap_ext_adv_set_addr()` validates
> the random-address subtype and **rejects resolvable-private addresses** (top-2-bits `01` / `0x40`)
> with `EINVAL` — only static (`11`) and NRPA (`00`) pass. Real privacy phones advertise with exactly
> the RPA subtype this milestone exists to imitate, so this blocks the headline feature. It is also a
> **pre-existing fleet bug**: on `main`, ~36% of BLE decoy identities (the RPA subtype) have silently
> failed `set_addr` and never advertised. Worse, `decoy_audit` scored the address-type mix as closed
> (~0.001) because it measures the *intended, host-side* identities from `synth_dump`, **not on-air
> reality** — a false-confidence gap this discovery corrects.
>
> **Fix (analogous to the Wi-Fi `en_sys_seq=false` byte-control bypass):** the controller transmits
> whatever six bytes it is handed; only the *host* enforces the subtype rule. So `churn_adv` sets a
> validation-passing **static stub** via the host API (which flips `rnd_addr_set` so `ext_adv_start`
> will not overwrite the address on enable), then **overrides** the controller's adv-set random
> address with the real bytes — including RPA `01` — via the raw `LE_SET_ADV_SET_RND_ADDR` HCI command
> (`ble_hs_hci_cmd_tx`, forward-declared since it lives in a private NimBLE header). To a *passive*
> scanner a fabricated `01` address that rotates is indistinguishable from a real IRK-derived RPA
> (resolution needs an IRK the attacker lacks). **Gate PASSED on-air:** a C5 promiscuous observer sees
> co-located decoys transmitting RPA (`01`), NRPA (`00`), and static (`11`) addresses, and the
> `set_addr rc=3` spam is gone. This fix also repairs the pre-existing `main` bug.

The remaining routine hardware expectation is that the controller accepts frequent adv-set random
address re-applies of all three subtypes at the rotation cadence — confirmed by the same on-air check.

## 7. Validation bar

Extend `tools/decoy_audit` from snapshot-only into the **temporal** slice its README already lists as
deferred ("lifespan cohort"), and re-capture on-air. Measured off a real BLE capture plus a labeled
synthetic run:

- **Rotation-cadence realism.** The distribution of inter-rotation intervals for RPA-subtype devices
  matches the real crowd's (heavy-tailed around ~15 min); static devices show zero rotations. Scored
  as JS divergence like the existing discriminators, with a regression gate.
- **Lifetime-cohort realism.** The transient/resident lifetime mixture matches the real crowd's
  presence-duration distribution; no single-band monoculture.
- **Rotation independence.** No synchronized rotation volley — across the run, address
  appearances/disappearances are not concentrated in shared instants.
- **Behavioural continuity.** Each rotation preserves interval/payload/company (verified directly on
  the labeled synthetic side; this is a *wanted* property, so it is asserted, not penalized).
- **No address resurrection.** A dead device's addresses never reappear later in the run.

## 8. Testing strategy

The pure `ble_devices` core is deterministic under a seeded RNG, so the host harness
(`tools/decoy_audit`, extended with a `--devices <seed> <n> <ticks> <tick_ms>` time-series dumper
mirroring `probe_audit`'s `--agents`) covers, without hardware:

- **Rotation cadence per subtype** — RPA devices rotate within the RPA band; NRPA within the NRPA
  band; static devices never rotate, across many seeds.
- **Behavioural continuity** — a device's payload/interval/company are byte-identical before and
  after a rotation; only `addr` changes, and only in the low 46 bits (subtype MSBs preserved).
- **Independent phase** — no tick rotates an implausibly large share of the live population
  simultaneously.
- **Lifetime bands + role split** — measured transient/resident ratio ≈ 70/30; transient lifetimes
  in 2–12 min, resident in 30–90 min; dead addresses never recur.
- **Rebirth freshness** — a reborn slot has a new address *and* independently re-drawn behaviour/role
  (no carry-over from the dead device).

On-target, a short serial-log smoke check on the C5 and C6 confirms the tick logs device
births/deaths and rotations, and that the controller accepts the rotation re-applies (no `set_addr`
rejections) at cadence. A BLE sniffer capture (the decoy-side analog of the Wi-Fi C5 sniffer) then
confirms on-air that a resident address rotates while its payload/interval persist, and that static
devices hold their address.

## 9. Non-goals (YAGNI)

- **No persona clusters.** Grouping devices into coherent phone+watch+earbuds PANs, intra-cluster
  co-presence, and cluster birth/death is **Milestone B**.
- **No ecosystem profiles.** Android/Apple/mixed content profiles (and the Apple-Continuity ethics
  decision) belong to Milestone B. This milestone keeps the existing generic content generator.
- **No physical-layer claims.** RSSI/AoA/co-location are out of scope and unfixable with this
  hardware (§3).
- **No raised concurrent-instance count as a guarantee.** Instantaneous device count stays bounded by
  the 4 ext-adv instances and airtime; density scaling is a tuning knob and a Milestone-B concern.
- **No capture-driven rotation parameters yet.** Rotation/lifetime bands ship as realistic constants;
  driving them from per-capture statistics is a documented later hook, not built here.
- **No new inter-board coordination.** Boards run `ble_devices` **autonomously** and are deliberately
  **un-synchronized** with each other.

## 10. Global constraints

- **Public repo.** No absolute local paths, OS usernames, real hardware MACs, or real SSIDs in any
  committed file. Captures and every parsed intermediate stay in the gitignored `private/`,
  PII-stripped; `decoy_audit` outputs remain aggregate-only, as today.
- **Targets.** ESP32-C5 (IDF 5.5) and ESP32-C6 (IDF 5.4); classic ESP32 CYD unaffected. The device
  core is target-agnostic; per-target population sizes stay compile-time as today.
- **Reuse, don't rewrite.** Content generation (`templates`/`generate`/`roster`) and the NimBLE apply
  (`churn_adv`) are reused; only the lifecycle/address layer is replaced. All existing decoy_audit
  snapshot discriminators must stay green (this milestone must not regress interval/vendor/address
  mix).
- **Host-testable core.** All new scheduling/rotation logic lives in a pure module with injected
  RNG/clock, unit-tested under `tools/decoy_audit`; NimBLE code stays out of the pure core.
- **All three subtypes are random addresses.** Rotation regenerates only the low 46 bits; the
  subtype MSBs are preserved and no real hardware MAC is ever exposed.

## 11. Open risks

- **Presenter re-apply on rotation** — `churn.c` must detect an occupant's address change (not just a
  device change) and re-apply, without thrashing the instance. Tuned so a rotation is a single clean
  re-apply.
- **Rotation cadence vs. airtime** — many residents rotating plus 70% transient churn means frequent
  `set_addr`/`set_data` re-applies; must stay within BLE's shared airtime budget under coexist.
  Bounded by the presenter, not the device core.
- **Lifetime/rotation tuning** — bands too short make devices vanish or rotate unrealistically fast (a
  tell); too long lets the crowd stabilize. Start with the §5 values, refine against a capture via
  the new temporal discriminators.
- **Superseding churn's dwell/cooldown** — retiring the active-set/cooldown lifecycle must not
  regress the snapshot audits or the webui/settings knobs that currently drive dwell/cooldown; those
  controls are remapped onto the new lifetime/rotation bands.
