# M4 — Per-vendor payload templates (joint) — Design

- **Status:** Approved design, pre-plan.
- **Builds on:** M3 (churn engine + roster, done & verified on ESP32-C6). M4 replaces the
  roster's payload *content* only; the churn/active-set/vessel machinery is untouched.
- **Source of truth:** `the project requirements handoff`, milestone M4.
- **Project:** Splinter → Simulacra (rename pending upstream sign-off).

## Goal

Replace `roster.c`'s current `company_id + random bytes` payloads with **structurally-valid,
coherently-bundled** device advertisements, so each decoy decodes as a believable real device
in a scanner. Vendor, advertising interval, and payload are chosen **jointly** (as one bundle)
so an impossible combination (e.g. a Garmin company ID with Tile timing) can never be produced.

## Refined Law 3 (safety — the iBeacon nuance)

Law 3 was "exclude Apple `0x004C` formats." That was a blanket proxy for the real hazard. The
hazard is **not** the Apple company ID — it is specific Apple **Continuity** subtypes:

| Subtype (in `4C 00 ..`) | Meaning | Pops up? |
|---|---|---|
| `0x02` (+ len `0x15`) | **iBeacon** (region beacon) | **No** — inert unless an app watches that exact UUID |
| `0x07` | Proximity Pairing (AirPods etc.) | **Yes** (the spam offender) |
| `0x0F` | Nearby Action | **Yes** (the spam offender) |
| `0x12` | Find My / offline finding | Triggers "unknown AirTag" alerts |

**Refined Law 3:** *emit only the iBeacon subtype (`0x02`/`0x15`); never emit Continuity
proximity-pairing (`0x07`), nearby-action (`0x0F`), or Find My (`0x12`).* Same safety guarantee,
precise instead of blanket — and it unlocks iBeacons (everywhere real RF collection happens:
retail lots, venues, transit). Enforced two ways: the iBeacon encoder hardcodes the `4C 00 02 15`
prefix (can't drift), and a self-test asserts no payload contains `4C 00` followed by
`{0x07,0x0F,0x12}`. All other laws (aggregates-only, no verbatim replay, non-connectable,
population-match, no-raw-capture) are unchanged.

## Believability scope (honest bar for M4)

Only the **open** formats (iBeacon, Eddystone) have public decoders, so those render as named
types in nRF Connect. The vendor blobs (earbuds/fitness/sensor) decode as *structurally-valid
manufacturer data for that vendor* — **not** byte-exact clones of proprietary payloads. Matching
real payloads exactly requires observation, which is M5→M6. **M4's bar is therefore: structurally
valid + format-correct + coherently bundled.** Not "indistinguishable from a specific real model."

## Architecture

New module `main/templates.{h,c}` (one responsibility: define device archetypes and render their
advertisement bytes). The churn engine, vessels, and `identity_t` are unchanged except that
`roster_init()` now sources payloads + interval from templates.

### `device_template_t`

```c
typedef enum {
    FMT_VENDOR_MFG,     // company_id + structured blob (earbuds / fitness / sensor)
    FMT_IBEACON,        // 4C 00 02 15 + UUID + major + minor + tx
    FMT_EDDYSTONE_UID,  // svc-data 0xFEAA frame 0x00
    FMT_EDDYSTONE_URL,  // svc-data 0xFEAA frame 0x10
    FMT_SVC_TRACKER,    // service-data tracker (Tile 0xFEED)
} fmt_family_t;

typedef struct {
    const char  *archetype;    // debug/inspection label
    fmt_family_t family;
    uint16_t     company_id;   // vendor-mfg & tracker-mfg families (0 otherwise)
    uint16_t     svc_uuid;     // service-data families (0xFEAA / 0xFEED)
    uint16_t     itvl_min_ms;  // joint interval band for this archetype
    uint16_t     itvl_max_ms;
    uint8_t      name_prob;    // % chance to attach a friendly name (0 = never)
    uint8_t      weight;       // mix proportion (relative)
} device_template_t;
```

A `static const device_template_t TEMPLATES[]` holds the rows below (several vendor rows reuse
`FMT_VENDOR_MFG` with different company IDs/names from `decoy_vendors.h`).

### Render path

- `const device_template_t *templates_pick(void)` — weighted random pick over `weight`.
- `void template_render(const device_template_t *t, struct ble_hs_adv_fields *f, uint8_t *scratch,
  uint16_t *itvl_ms)` — fills `f` (and `scratch` for mfg/svc-data bytes) for the chosen family,
  and returns a concrete interval sampled from `[itvl_min_ms, itvl_max_ms]`. Dispatches to a small
  per-family encoder.
- **Serialization is unchanged:** `roster_init()` still calls `ble_hs_adv_set_fields(f, ...)` to
  produce the frozen 31-byte payload, exactly as today — `template_render` just fills the fields
  struct correctly per format (NimBLE's `ble_hs_adv_fields` already supports `mfg_data`,
  `svc_data_uuid16`, and the `uuids16` list, so beacons serialize through the same path).

## The 7 archetypes (concrete encoder layouts)

1. **iBeacon** — `FMT_IBEACON`, company `0x004C`. mfg_data = `4C 00 02 15` + 16-byte random UUID +
   2-byte major + 2-byte minor + 1-byte measured power (`0xC5` = −59 dBm). 25 mfg bytes; no name.
   ~100 ms.
2. **Eddystone-UID** — `FMT_EDDYSTONE_UID`, svc `0xFEAA`. `uuids16=[0xFEAA]` (complete) +
   svc_data = `AA FE 00` (frame) + 1-byte tx + 10-byte namespace + 6-byte instance + `00 00`.
   ~100 ms.
3. **Eddystone-URL** — `FMT_EDDYSTONE_URL`, svc `0xFEAA`. svc_data = `AA FE 10` + tx +
   1-byte scheme (`0x03`=https://) + URL bytes from a small curated benign list, using Eddystone
   expansion codes (e.g. `0x07`=".com/"). ~700 ms.
4. **Wireless earbuds** — `FMT_VENDOR_MFG`, company Samsung `0x0075` / Bose `0x009E` / Sony
   `0x012D`. mfg_data = company + {model byte, status byte, battery 0–100, 1–3 plausible bytes}.
   `name_prob` ~60 (e.g. "Galaxy Buds"). ~150 ms.
5. **Fitness band/watch** — `FMT_VENDOR_MFG`, Garmin `0x0087`. mfg_data = company + {model, status
   flags, battery, 1–2 bytes}. `name_prob` ~50 ("vivosmart"). ~1 s.
6. **Tile tracker** — `FMT_SVC_TRACKER`, svc `0xFEED`. `uuids16=[0xFEED]` + svc_data = `ED FE` +
   ~10-byte tracker-id-shaped blob. No name. ~1–2 s.
7. **Nameless BLE sensor** — `FMT_VENDOR_MFG`, Nordic `0x0059`. mfg_data = company + 2–4 sparse
   bytes. No name. ~2 s.

### Starting mix weights (tunable; M6 later learns the real mix)

earbuds 22, sensor 18, iBeacon 16, fitness 14, Tile 14, Eddystone-UID 10, Eddystone-URL 6.
(Roughly: wearables/earbuds common, beacons moderate, trackers/sensors fewer.)

## Integration with `roster.c`

`roster_init()` per identity becomes: `make_random_static_addr(addr)` → `t = templates_pick()` →
`template_render(t, &f, scratch, &itvl)` → `ble_hs_adv_set_fields(&f, …)` → freeze `payload`,
`payload_len`, `company_id` (from `t`), `adv_itvl_ms = itvl`. The current `build_fields()` and the
`SIMULACRA_NAME_PROB/MFG_PROB` macros are replaced by the template path (per-template `name_prob`
supersedes the globals). `identity_t` gains an optional `uint8_t archetype_idx` for inspection/test.

## Testing

**Proving (self-test) — new structural assertions over the whole roster:**
- Every `FMT_IBEACON` payload begins `4C 00 02 15` and is the right length.
- Every Eddystone payload carries svc-UUID `0xFEAA` with a valid frame byte (`0x00`/`0x10`).
- Every Tile payload carries svc-UUID `0xFEED`.
- No payload exceeds 31 bytes; `payload_len > 0` for all.
- Each identity's `adv_itvl_ms` lies within its template's band.
- **Law-3 guard:** no payload contains `4C 00` followed by `0x07`/`0x0F`/`0x12`.
- Existing roster/churn/cooldown/time-slice self-tests still pass (logic unchanged).

**Manual acceptance (on the C6):** nRF Connect scan shows iBeacons decoding as iBeacon
(UUID/major/minor), Eddystones decoding as Eddystone, and vendor blobs as plausible named-vendor
manufacturer data. No pairing pop-ups on a nearby phone over a multi-minute scan.

## Scope / boundaries

- Keep `templates.c` under 500 lines; if the encoders grow, split them into `template_encoders.c`.
- **Out of scope (M5/M6):** any live capture or learning. M4 is entirely hand-written templates
  with hand-set mix weights.

## Acceptance criteria

1. Decoys decode cleanly as their claimed format in nRF Connect; open formats (iBeacon/Eddystone)
   render as named types. ✅ manual.
2. No vendor/payload/interval mismatches — every decoy is a coherent bundle. ✅ self-test.
3. No pairing pop-ups; no forbidden Apple subtype ever emitted. ✅ self-test guard + manual.
4. Churn behaviour unchanged from M3 (population still persists/turns over). ✅ regression.
