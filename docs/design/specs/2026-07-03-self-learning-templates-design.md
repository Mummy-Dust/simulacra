# Self-Learning Templates (Phase 1) — Design

**Date:** 2026-07-03
**Status:** Design approved, ready for implementation plan
**Scope:** On-device only. Fleet SD-backed sharing is Phase 2 (see
`2026-07-03-fleet-template-sync-design.md`).

## Summary

Today the decoy's synthetic crowd is model-driven for the *mix* (which vendors,
how many, how often — from the M5 `rf_model`) but every emitted advertisement's
*structure* comes from one of nine hand-authored archetypes in `templates.c`
(or the generic vendor-mfg encoder). This feature adds a **self-learning
harvester**: while the decoy passively scans, it captures the *shape* of real
nearby devices' advertisements, strips all identity, and turns them into new
learned templates that feed the same churn/roster engine — so the fake crowd
matches whatever real gear is actually around.

The make-or-break constraint: **learn structure, never identity.** A learned
template is "a device *of this vendor / format / cadence*", never "*this exact
device*."

## Goals

- Capture the AD-skeleton (TLV layout) of persistent nearby devices and render
  new decoy identities from it, with all identifying bytes randomized per
  emission.
- Only learn devices seen repeatedly and parsing cleanly (no one-off / garbled
  captures polluting the crowd).
- Persist a growing-but-bounded library across reboots, with age-out so shapes
  from places the device has left fade rather than travel indefinitely.
- Never emit a forbidden subtype (Law 3) — apply the same guard to learned
  templates as to hand-authored ones.
- Keep the core logic host-testable (no radio) in the existing `churn_selftest`
  harness.

## Non-goals (this phase)

- Cross-device sharing / SD storage / ESP-NOW sync — Phase 2.
- Wi-Fi probe-shape learning — deferred (pairs with the Wi-Fi observe→match
  milestone).
- Clustering multiple sightings to statistically derive an "invariant" format —
  we use a repeat-sighting eligibility bar plus a single clean capture, not
  multi-sample clustering.

## Architecture

New module `learn.{c,h}`. One thin hook added to `observe.c`. The Law-3 guard is
promoted out of `churn_selftest.c` into a shared header so both the selftest and
`learn.c` use one implementation.

```
observe_gap_event (parses AD, has raw bytes + company, then drops the MAC)
        │  learn_offer(mac_hash, raw_ad, len, company, svc_uuid, now_ms)
        ▼
learn.c candidate table  ──  per-sweep, RAM, keyed by mac_hash (never the MAC)
        │  count sightings of the same shape; accumulate interval band
        │  when count >= LEARN_MIN_SIGHTINGS within a sweep:
        ▼
   strip + Law-3 gate  →  promote or reinforce in the learned store
        ▼
learned store (RAM, capped, NVS-backed)  ──►  generate.c build_for_vendor()
```

`observe.c` / `rf_model` retain their purpose (modeling *distributions*).
`learn.c` is a parallel consumer of the same scan reports that owns *structure*.

## Components

### 1. Capture hook (`observe.c`)

A single call added in `observe_gap_event`, right before the MAC is dropped, on
the path that already parsed `struct ble_hs_adv_fields`:

```c
learn_offer(observe_hash_mac(d->addr.val), d->data, d->length_data, company, now);
```

- The candidate key is the **already-computed, non-reversible hash** — no MAC
  reaches `learn.c`.
- Only `company_id` (already parsed by observe) is passed as a hint; `learn.c`
  re-parses the raw AD for the full skeleton and derives `svc_uuid` and family
  itself, so no additional parsing is added to `observe.c`.
- Gated by `SIMULACRA_LEARN` (build flag; see Configuration).

### 2. Candidate table (`learn.c`, internal)

- Bounded table (`LEARN_CAND_SLOTS`, default 24), keyed by `mac_hash`.
- First offer for a hash stores a candidate: the raw (not-yet-stripped) AD, a
  sighting count of 1, and the first interval sample.
- Repeat offers with a matching shape bump the count and widen the interval
  band.
- When `count >= LEARN_MIN_SIGHTINGS` (default 3) within one observe sweep, the
  candidate is run through the strip + Law-3 gate and promoted/reinforced.
- The candidate table is **wiped each sweep** (like observe's dedup table). A
  device too slow to reach K sightings in one ~15 s sweep is simply not learned
  — acceptable; K and the sweep scope are tunable.
- The transient raw AD in a candidate slot is the only place pre-strip bytes
  live; it is RAM-only and wiped per sweep, exactly like observe holding MACs
  transiently.

### 3. Identity-strip + Law-3 gate (`learn.c` + shared `law3.h`)

Converts a clean parsed AD into a **skeleton + randomize-mask** by walking the
TLV structure. Per AD type:

| AD type | Policy |
|---|---|
| Flags `0x01` | keep verbatim |
| Service UUIDs `0x02`–`0x07` | keep verbatim (denote a service/protocol, not identity) |
| Local Name `0x08`/`0x09` | replace-on-render: synthetic name from a generic pool, same length class; drop if no fit |
| TX Power `0x0A` | keep (dither allowed) |
| Appearance `0x19` | keep |
| Mfg Data `0xFF` | keep company_id (first 2 bytes); mask the blob. iBeacon: keep `4C 00 02 15` prefix, mask UUID/major/minor |
| Service Data `0x16`/`0x20`/`0x21` | keep the UUID; mask the rest |
| unknown / other | keep type+length, mask value bytes |

"Mask" = mark the byte position in a `uint32_t rand_mask` (bit set → rewrite
with `esp_random()` on every render). AD is <=31 bytes so 31 bits suffice.

**Law-3 reject gate** — the whole capture is dropped, never learned, if:
- Apple (`0x004C`) mfg-data with subtype `0x07` / `0x0F` / `0x12` (Continuity
  pairing / nearby action / Find My) — via the promoted `has_apple_popup_subtype`.
- Microsoft Swift Pair (`0x0006` beacon format).
- Google Fast Pair (service-data `0xFE2C`).
- Fails to re-serialize, or exceeds 31 bytes after stripping.

**Shape dedup:** `shape_hash` over `(family, company_id, svc_uuid, AD-type/length
sequence)` — **not** value bytes. A matching shape reinforces the existing entry
instead of adding a duplicate.

### 4. Learned store (`learn.c`)

Fixed array `learned_template_t store[LEARN_CAP]`, a flat pointer-free POD so it
is directly serializable (Phase 2 ships it over the wire unchanged):

```c
typedef struct {
    uint8_t  ad[31];            // identity-stripped skeleton
    uint8_t  ad_len;
    uint32_t rand_mask;         // bit set => re-randomize ad[i] on render
    uint16_t company_id;        // 0 if none
    uint16_t svc_uuid;          // 0 if none
    uint8_t  family;            // fmt_family_t classification
    uint16_t itvl_min_ms;
    uint16_t itvl_max_ms;
    uint32_t shape_hash;        // dedup key
    uint16_t reinforce_count;   // quality / weight
    uint16_t last_seen_sweep;   // for age-out
} learned_template_t;           // ~56 bytes
```

- `LEARN_CAP`: **Ward (C5) = 128, Shade (C6) = 64** (persona `#define`). RAM
  ~7 KB / ~3.6 KB. The cap is headroom — the real ceiling is how many distinct
  shapes actually exist nearby.
- **NVS** persistence mirrors `rf_model_save_nvs`/`load_nvs`: namespace
  `"splinter"`, key `"learn_db"`, blob prefixed with `magic + version`. Saved
  debounced on promote/reinforce and on the reprofile persist tick; loaded on
  boot.

### 5. Lifecycle

- **Reinforce:** re-observing a known shape bumps `reinforce_count`, refreshes
  `last_seen_sweep`, widens the interval band.
- **Age-out:** a shape not re-observed within `LEARN_AGEOUT_SWEEPS` is evicted,
  so shapes from a left location fade.
- **Eviction when full:** a new qualifying shape with no free slot evicts the
  weakest — lowest `reinforce_count`, tie-broken by oldest `last_seen_sweep`.

### 6. Generation integration (`generate.c`)

- `build_for_vendor` gains the learned store as an additional source: a roster
  entry sampling `company` may draw a learned template matching that
  company/family (multiple learned shapes per vendor = more variety), else falls
  through to today's static-template logic. Learned templates are also eligible
  on the no-mfg and generic paths. Selection weighted by `reinforce_count`.
- **`archetype_idx` extension (offset scheme):** `idx < templates_count()` →
  static template; `idx >= templates_count()` → `learned[idx -
  templates_count()]`. The `churn_selftest` invariant
  `archetype_idx < templates_count()` is updated to the new upper bound.
- **Render** copies the skeleton, rewrites `rand_mask` bytes with `esp_random()`,
  and injects a fresh synthetic name. The roster-wide `has_apple_popup_subtype`
  assertion stays as belt-and-suspenders even though the gate already guarantees
  cleanliness.

## Data discipline (Law 3 / opsec)

- No MAC, name, serial, UUID, or other unique field is ever stored — only the
  format skeleton with identifying positions masked.
- Every render produces different value bytes and a different synthetic name:
  a learned template is a *class* of device, never a clone of a specific one.
- Forbidden subtypes are never learned (reject gate) and never emitted (render
  gate + roster assertion).

## Error handling

- Candidate table full → new candidates dropped for that sweep (bounded, no
  allocation).
- Strip failure / re-serialize failure / over-budget → capture rejected.
- NVS load of a stale/corrupt blob (bad magic/version) → treated as empty; store
  rebuilds from live observation.
- Store full with no evictable entry weaker than the candidate → candidate not
  admitted (existing library preferred over churn).

## Testing

Host-runnable in `churn_selftest` (no radio):

- **strip:** known ADs (earbud mfg, iBeacon, Eddystone, named watch) → name
  replaced with synthetic, company_id preserved, blob bytes masked.
- **Law-3 gate:** Apple Continuity/nearby/Find My, Swift Pair, Fast Pair → all
  rejected.
- **eligibility:** `<K` sightings → not promoted; `>=K` clean → promoted;
  garbled parse → rejected.
- **dedup/reinforce:** same shape twice → one entry, `reinforce_count` bumped.
- **render:** `<=31 B`, non-connectable, Law-3-clean; two renders share
  structure but differ in value bytes.
- **lifecycle:** cap enforcement + age-out picks weakest-then-oldest.
- **NVS round-trip**, and extended whole-roster invariants (rosters including
  learned entries are all-clean, all valid archetype indices).

On-target: soak observe near real devices; confirm learned entries populate and
rosters draw from them.

## Configuration

| Symbol | Default | Meaning |
|---|---|---|
| `SIMULACRA_LEARN` | on (both personas) | build gate for the whole feature |
| `LEARN_CAP` | Ward 128 / Shade 64 | learned store capacity |
| `LEARN_CAND_SLOTS` | 24 | candidate table size |
| `LEARN_MIN_SIGHTINGS` | 3 | K sightings to qualify |
| `LEARN_AGEOUT_SWEEPS` | tuned to ~hours | eviction age |

The modular option of confining harvesting to a dedicated observe/sniffer node
(rather than every decoy) is left open for a future role split; default is that
any decoy running observe also learns.

## Forward-compatibility with Phase 2

`learned_template_t` is a flat, pointer-free, `shape_hash`-keyed POD precisely so
Phase 2 can ship it over ESP-NOW and merge it across nodes with no rework. The
store is accessed behind a thin add / reinforce / evict / iterate / serialize
interface so its backing (NVS today) can be extended to SD + sync later.
