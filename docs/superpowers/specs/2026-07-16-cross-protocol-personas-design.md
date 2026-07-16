# Cross-Protocol Personas — Design

**Date:** 2026-07-16
**Status:** Approved (design), pending implementation plan
**Roadmap:** Phase 4 / M10 — Cross-protocol personas

## Goal

Bind a BLE identity and a Wi-Fi (probe-request) identity into a single synthetic
"device" — a **persona** — that is born together, present together, and leaves
together, with a coherent device type across both radios.

This serves the project north-star (counter-surveillance by obfuscation: hide the
user's real devices in a plausible fake crowd). A cross-modal correlator such as
Leonardo ELSAG **SignalTrace** links BLE and Wi-Fi observations by spatial/temporal
co-occurrence and can *filter out* single-radio "ghosts" (a BLE device with no
matching Wi-Fi presence is not a phone). That filtering step isolates the user's
**real dual-radio phone**, which *is* a genuine BLE+Wi-Fi co-occurrence. Personas
manufacture a crowd of fake dual-radio devices so the real phone is no longer the
unique co-occurring pair.

## Threat model (what this does and does not beat)

- **Defeats:** the "single-radio ghost" filter and the "this BLE device has no
  Wi-Fi twin, so it's not a real phone" heuristic. After personas, the Wi-Fi crowd
  and the BLE crowd share a realistic set of co-present, co-terminating pairs.
- **Does not beat (honest ceiling, this feature):** on a single board every persona
  shares one physical radio and one location, so all personas have an identical RF
  hardware fingerprint and identical RSSI/motion. A correlator with RF-fingerprinting
  gear or fine RSSI still sees "many dual-radio devices at one point." Defeating the
  **co-location** tell requires spreading a persona's two radios across two physically
  separated mesh nodes — **mesh-distributed personas (v2)**, out of scope here. v1 is
  same-board.
- **Additive only:** cannot silence the user's real device. Strongest posture remains
  Simulacra + emission hygiene.

## Design laws (this feature respects all five)

1. Aggregates only — no per-device real identifiers stored or replayed.
2. No verbatim replay — every persona identity is freshly synthesized.
3. **Non-connectable / no pairing-popup formats** — see the Apple/Law-3 constraint below.
4. Population-match — personas are a realistic minority (phone-like) of a crowd that
   is mostly BLE-only; active fakes never far exceed observed real density.
5. No raw capture in shipping builds — unchanged.

## Composition

- **Bind from the Wi-Fi side.** Almost every real device that emits Wi-Fi probes
  (a phone/laptop) also emits BLE, but many BLE devices (beacons, tags, fobs) emit
  no Wi-Fi. So **every probe agent is bound into a persona** (persona count = probe
  agent count, ~8–16), while extra **BLE-only** decoys (`ble_devices`) fill out the
  crowd. This keeps the population realistic and bounds complexity.
  **Composition (decided 2026-07-16):** every probe agent is bound (`n_persona = probe_agent_count`);
  the BLE population is *grown* to `probe_agent_count + ~8` so all personas get a co-present BLE
  twin AND an unbound remainder preserves the static/NRPA/persistent BLE-only mix (i.e. the
  address-type + presence tells we already closed do not regress). On the C5 that is 16 personas +
  8 BLE-only = 24; on the C6, 8 + 8 = 16 (both within `BLE_DEVICES_MAX` = 32).
- **Persona BLE members are always RPA** (resolvable private address) — that is how
  phones present on BLE — so they rotate their address within life. Co-presence is by
  *time*, not by address; rotation within a persona's life is realistic and does not
  break the linkage.

## Coherent archetype + the Apple/Law-3 constraint

A persona has a **family** that maps to *both* a Wi-Fi probe archetype and a BLE
behaviour:

| Family  | Wi-Fi archetype | BLE behaviour                                             |
|---------|-----------------|----------------------------------------------------------|
| SAMSUNG | `ARCH_GALAXY`   | vendor **matches**: company 0x0075 (from `VENDORS`)       |
| GOOGLE  | `ARCH_PIXEL`    | vendor **matches**: company 0x00E0 (from `VENDORS`)       |
| APPLE   | `ARCH_IPHONE`   | **Law-3-safe anonymous RPA** (flags/appearance; no Apple mfg data) |
| GENERIC | `ARCH_ANDROID`  | generic RPA / no-mfg shape                                 |

**Why Apple is special:** the BLE vendor palette (`decoy_vendors.h`) deliberately
excludes Apple (0x004C) because its Continuity payload triggers AirPods/Find-My
pairing pop-ups on bystanders' phones — a Law-3 violation. A fully vendor-matched
Apple persona is therefore impossible on BLE, and Law-3 is a hard gate that wins.

**This is not a contradiction.** A real iPhone spends most of its time emitting a
plain rotating RPA with *no* Continuity manufacturer data — exactly the Law-3-safe
shape above. "iPhone Wi-Fi probes + an anonymous co-present RPA on BLE" reads as a
real phone to a correlator, because the cross-modal link it makes is **temporal
co-occurrence**, not "both radios advertise the same vendor." Samsung and Google
personas get the stronger vendor-matched coherence for free because those companies
are in the palette.

Every persona BLE member passes the existing Law-3 gate at construction, same as
any other decoy.

## Architecture (Approach B — shared persona registry)

The two existing population cores (`ble_devices`, `probe_agents`) are pure and
independently host-tested. Approach B preserves them and adds two small pure modules
rather than rewriting them.

### New module: `main/uniq_id.{h,c}` — guaranteed-unique identity allocator

Every fresh address on **both** radios (BLE births/rotations, Wi-Fi births) is drawn
through this allocator so no freshly-emitted identity collides with any live-or-recent
one.

- State: a ring buffer of the last `UNIQ_HISTORY` issued 6-byte addresses. `UNIQ_HISTORY`
  is sized well above the max concurrent live population (default 2048 ≈ 12 KB) so every
  currently-live address is always still in the ring — one structure covers both
  "not live" and "not recently retired."
- `void uniq_reset(void);`
- `void uniq_gen_addr(uint8_t out[6], uint8_t top2);` — fill `out` with a random address
  whose top two bits are `top2` (address-type selector), not present in the ring; record
  it (evicting the oldest). Redraw on the astronomically rare collision.
- **Guarantee scope (honest):** unique across all live identities + the last `UNIQ_HISTORY`
  retired. Beyond that window, a 46-bit random draw recurs with probability ~2⁻⁴⁶ —
  effectively never, and never as a *live* duplicate.
- **Not** a constraint on a single device repeating its own advert — a beacon re-broadcasting
  identical bytes is correct real behaviour, not a duplicate.

`make_random_addr` (used by `ble_devices` and `roster`) is routed through `uniq_gen_addr`;
`probe_agents` MAC generation is routed through it too.

### New module: `main/persona.{h,c}` — the linkage core (pure)

Owns only the shared lifecycle + family, not the radio identities themselves.

```c
typedef enum { PF_SAMSUNG, PF_GOOGLE, PF_APPLE, PF_GENERIC, PERSONA_FAMILY_COUNT } persona_family_t;

typedef struct {
    persona_family_t family;
    uint32_t born_ms;
    uint32_t life_ms;
    uint32_t generation;   // bumped on each reincarnation; how bound members detect a new life
    bool     alive;
} persona_t;

#define PERSONA_MAX PROBE_AGENTS_MAX     // one persona per probe agent

void  persona_init(int n, uint32_t now_ms);       // create n personas (<= PERSONA_MAX)
int   persona_lifecycle(uint32_t now_ms);         // retire+reincarnate expired; returns # reborn
int   persona_count(void);
const persona_t *persona_at(int i);
probe_arch_t     persona_arch(persona_family_t f);    // family -> Wi-Fi archetype
```

Persona lifetime uses a phone-like band (a persona is a person's phone passing through,
or lingering). On reincarnation the family is redrawn (weighted to a realistic phone mix)
and `generation` is incremented.

### Changes to existing cores (minimal, additive)

- **`probe_agent_t`** gains `uint32_t persona_gen;`. Agent `i` is bound to persona `i`
  (index-parallel; every agent is a persona).
- **`ble_device_t`** gains `int8_t persona_idx;` (-1 = unbound) and `uint32_t persona_gen;`.
  The `ble_devices` array reserves slots `[0, n_persona)` as bound; `[n_persona, n)` are the
  existing free/unbound crowd.
- **`persona_sync`** (a new step, lives in `persona.c` or the integration site): for each
  bound member whose `persona_gen != persona[i].generation`, reincarnate it to match its
  persona — adopt `born/life`, set the family's arch (Wi-Fi) or vendor/behaviour (BLE),
  draw a fresh unique identity, and copy the new `generation`. Bound members do **not**
  expire independently; the persona owns their life.
- `ble_devices_tick` skips bound slots (persona drives them) and ticks unbound slots exactly
  as today — including within-life RPA rotation for bound members via the existing rotation
  path (rotation still allowed; it draws a fresh unique address).
- The `SIMULACRA_PROBE` standalone injector path (seq-gate) is untouched; personas exist only
  in the normal decoy build.

### Data flow per scheduler tick

1. `persona_lifecycle(now)` — expire + reincarnate personas (bump generations).
2. `persona_sync(now)` — align each bound BLE + Wi-Fi member to its persona.
3. `ble_devices_tick(now)` — the unbound BLE crowd (birth/death/rotation), as today.
4. Probe injection pulls the due agent subset (unchanged), now archetype-driven by personas.

Net effect: each persona's BLE and Wi-Fi members appear together, are continuously present
for the same window (rotating addresses within it, like a real phone), and disappear together.

## Observability

- **`decoy_audit`** gains a **dual-radio coverage** axis: the fraction of the Wi-Fi crowd
  that has a co-present BLE twin, scored against a real phone-heavy environment. (A real
  crowd has many such pairs; a decoy fleet with zero would be separable — the same
  audit-first discipline as every other tell.) Exact metric defined in the plan.
- **Vigil** can surface persona count on the node card (piggybacks the existing status wire;
  a spare byte, no struct-size growth if a reserved field exists — confirmed in the plan).

## Testing (all pure, host-compiled like existing cores)

- `uniq_id`: no collision with live set under heavy churn; no repeat within the history
  window; correct top-2 bits preserved; redraw-on-collision terminates.
- `persona`: lifecycle reincarnation bumps generation and redraws family; family→arch map;
  family mix over many reincarnations matches the intended weights.
- `persona_sync`: a bound BLE member and its Wi-Fi member share born/life and reincarnate
  together on a generation bump; addresses differ across the two radios; bound members do
  not expire independently.
- Coherence: Samsung/Google personas carry the matching BLE company; Apple/generic carry a
  Law-3-safe RPA with no forbidden mfg data.
- Law-3: every persona BLE member passes `law3_forbidden` at construction.

## Out of scope (future)

- **Mesh-distributed personas (v2):** split a persona's two radios across two physically
  separated mesh nodes so the pair no longer shares one RF fingerprint / one location.
  This is the hardening that beats the co-location tell; it needs an ESP-NOW coordination
  protocol and is its own spec.
- Activity/motion coupling (full behavioral coherence) — needs motion data a stationary
  board cannot source; deferred.
