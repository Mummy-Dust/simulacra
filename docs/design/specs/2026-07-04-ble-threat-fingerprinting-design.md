# BLE Threat Fingerprinting (Slice 1) — Design Spec

**Date:** 2026-07-04
**Status:** Approved for planning
**Feature line:** M10 known-tracker detection / surveillance fingerprinting (see memory
`detect-surveillance-fingerprints`). This is **slice 1 of a decomposed roadmap.**

## Goal

Recognize specific BLE device *classes* by RF signature — starting with consumer trackers
(AirTag/Find My, Samsung SmartTag, Tile) — and surface them as typed "known device" threats on
Vigil. Detection/awareness only; never interference, jamming, or evasion. This is the defensive
mission: know what's watching you, a counter to passive collectors like Leonardo SignalTrace.

## Scope

**In scope (slice 1):**
- A packed, versioned, categorized BLE signature record format.
- A pure per-advert matcher on the decoy, hooked into the existing observe pipeline.
- Integration into `detect.c` as a new `DETECT_KNOWN` threat kind sharing the existing threat table.
- An encrypted-at-rest signature DB on Vigil's SD, distributed to decoys over ESP-NOW (the inverted
  `learn.db` librarian: Vigil authors/custodies, decoys consume and match).
- A compile-time seed signature set so decoys detect standalone.
- Vigil display of known-device hits with an honest confidence qualifier.

**Out of scope (later slices, explicitly deferred):**
- Wi-Fi / SSID / OUI fingerprints (Flock-type cameras, device-name patterns).
- Surveillance/LE classes beyond BLE trackers (Axon body cameras, dashcams) — the format supports
  them via `category`, but the seed set is trackers only.
- Cross-rotation persistence / escalation to "following you" — owned by the future
  `threat-history-escalation` feature. A hit here means "class present in the area," not identity.
- Sub-GHz (TPMS/CC1101), NFC/RFID, and cellular (IMSI-catcher) threats — different hardware, new
  node types. The C5/C6 radios are blind to them; the product must say so.
- A web-UI / OTA path for editing signatures (the SD/ESP-NOW path is the slice-1 update mechanism).

## Architecture

The signature DB is the `learn.db` librarian **inverted**. With learning, decoys harvest ambient
advert *shapes* and push them *up* to Vigil, which aggregates and rebroadcasts. With fingerprinting,
Vigil custodies an *authored* signature DB and pushes it *down* to decoys, which match it against
what they passively scan.

**Matching must run on the decoy** — it is the only board with the passive BLE scanner and raw
adverts (`observe_gap_event`). Vigil (classic ESP32, receive-only display + SD) never sees adverts.

Roles:
- **Decoy (Ward/Shade):** holds a signature DB in RAM (seeded from firmware, optionally updated by a
  Vigil push); matches each observed advert; raises a `DETECT_KNOWN` hit that rides the *existing*
  status frame back to Vigil.
- **Vigil (CYD librarian):** custodian of the canonical encrypted signature DB on SD; pushes it to
  decoys over ESP-NOW on a slow re-announce; displays the hits.

Graceful degradation (same principle as learn): a decoy with no Vigil runs its compile-time seed DB
at full capability. Vigil is purely additive.

## Global Constraints

- **Law-3 / data discipline:** a hit records a *category/class*, never a raw identity. Storage is
  hash-only, exactly like `detect_threat_t` today.
- **Never trust the wire:** every signature received over ESP-NOW is re-gated (bounds + range checks)
  before entering the RAM DB. The matcher also bounds-checks defensively. A leaked PSK cannot turn a
  pushed signature into an out-of-bounds read — worst case is a missed or spurious *match*.
- **Packed, pointer-free PODs:** `threat_sig_t` is wire- and SD-ready, same discipline as
  `learned_template_t`.
- **Paired builds:** decoy and Vigil are always flashed together, so the persisted + wire threat
  struct format bump is coordinated. Bump the format version regardless.
- **Honesty in-product:** every hit is qualified (`likely` / `possible`); the docs state the
  area-presence (not "following you") semantics and the cellular/sub-GHz blind spot.
- Commits carry AI trailers → scrub before any push (repo policy). `main` is local-only.
- Selftest builds: `-DCHURN_SELFTEST=1`, and pass `-DCHURN_SELFTEST=0` explicitly for normal builds
  (stale CMake cache gotcha).

## Data Model

### Signature record

```c
#define SIG_PAT_MAX 16                      // max masked-pattern length

typedef enum { SIG_CAT_TRACKER = 0, SIG_CAT_CAMERA, SIG_CAT_BODYCAM, SIG_CAT_UNKNOWN } sig_category_t;
typedef enum { SIG_CLASS_AIRTAG = 0, SIG_CLASS_SMARTTAG, SIG_CLASS_TILE /* … */ } sig_class_t;
typedef enum { SIG_SRC_MFG_DATA = 0, SIG_SRC_SVC_DATA } sig_src_t;

// BLE address types accepted, as a bitmask (0 = any).
#define SIG_ADDR_PUBLIC  0x01
#define SIG_ADDR_STATIC  0x02   // random static
#define SIG_ADDR_RPA     0x04   // resolvable private (rotating)
#define SIG_ADDR_NRPA    0x08   // non-resolvable private

typedef struct __attribute__((packed)) {
    uint16_t sig_id;            // stable id (reporting / DB provenance)
    uint8_t  category;          // sig_category_t
    uint8_t  class_id;          // sig_class_t
    uint16_t company_id;        // required mfg company id; 0xFFFF = don't care
    uint16_t svc_uuid16;        // required 16-bit service UUID; 0x0000 = don't care
    uint8_t  addr_type_mask;    // SIG_ADDR_* bitmask; 0 = any
    uint8_t  match_src;         // sig_src_t — which AD structure the pattern reads
    uint8_t  pat_off;           // offset into that AD payload
    uint8_t  pat_len;           // pattern length (<= SIG_PAT_MAX)
    uint8_t  pattern[SIG_PAT_MAX];   // expected bytes
    uint8_t  mask[SIG_PAT_MAX];      // 0xFF = must-match byte, 0x00 = ignore (rotating)
    uint8_t  confidence;        // 0..100 quality tier for the UI
} threat_sig_t;                 // ~45 B
```

### Signature DB (versioned unit)

```c
#define SIG_DB_MAGIC   0x53494731u   // "SIG1"
#define SIG_DB_VERSION 1             // on-disk/on-wire format version
#define SIG_DB_CAP     64            // RAM working set on both roles

// The DB is one versioned unit. `content_version` is the authored-content revision
// (bumped when signatures change); whole-DB newest-wins, no per-record merge.
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format_version;     // SIG_DB_VERSION
    uint16_t content_version;    // authored revision (monotonic)
    uint16_t count;              // number of threat_sig_t following
    uint8_t  reserved[2];
} sig_db_hdr_t;                  // authenticated as GCM AAD
```

## Components (file structure)

Shared (compile for both classic ESP32 / IDF 5.4 Vigil and C5/C6 decoy), under
`components/simulacra_radar/`:

- **`threat_sig.h`** — `threat_sig_t`, category/class/src enums, `SIG_*` constants, the extracted
  advert-fields struct the matcher consumes.
- **`sig_match.{h,c}`** — pure matcher. `sig_match(const sig_adv_fields_t *adv, const threat_sig_t *db,
  size_t count, sig_hit_t *out)` → bool. No radio, no allocation. Bounds-checks `pat_off + pat_len`
  against the advert payload length on every compare.
- **`sig_regate.{h,c}`** (or fold into `sig_wire.c`) — `sig_regate(const threat_sig_t *)`: validate
  `pat_len <= SIG_PAT_MAX`, `pat_off + pat_len <= SIG_PAT_MAX`, `category`/`class_id`/`match_src` in
  range, `confidence <= 100`. Applied on every wire receipt.
- **`sig_db.{h,c}`** — near-clone of `learn_db.{h,c}`: `sig_db_seal` / `sig_db_open` (AES-256-GCM,
  `"SIG1"` magic, header as AAD), `sig_db_derive_key` (HKDF-SHA256 from the fleet PSK). Atomic write
  helpers live on the Vigil side (mirroring learn.db save).
- **`sig_wire.{h,c}`** — mirror of `learn_wire.{h,c}`: `RADAR_TYPE_SIG_SYNC` frame, chunk pack/unpack
  (`SIG_WIRE_RECS_PER_CHUNK` ≈ 4), each chunk tagged with `content_version` + chunk index/count.
- **`sig_class_name.h`** (tiny) — `class_id → const char*` for Vigil labels.

Decoy (`main/`):
- **Observe hook** (`observe.c`): after parsing, build `sig_adv_fields_t` and call `sig_match`; on a
  hit, call the new `detect_note_known(...)`.
- **`esp_now_link.c`:** handle `RADAR_TYPE_SIG_SYNC` — reassemble chunks of a `content_version`, re-gate
  each record, and when a complete version ≥ current arrives, swap the RAM DB wholesale.
- **Seed DB** (`sig_seed.c` or a `const` table in `threat_sig.h`): the compile-time signature set,
  loaded into the RAM DB at boot.

Vigil (`cyd/main/cyd_main.c`):
- Load the on-card DB (or write the compiled seed if absent/older), hold it in RAM.
- Broadcast `SIG_SYNC` on a slow re-announce (~60 s).
- Render known-device hits on the DETAIL + radar pages.

`detect.c` / `detect.h`:
- Extend `detect_threat_t` with `kind` (`DETECT_KIND_FOLLOWER` / `DETECT_KIND_KNOWN`), `class_id`,
  `category`, `confidence`.
- `detect_note_known(uint32_t hash, int8_t rssi, uint8_t class_id, uint8_t category,
  uint8_t confidence, uint16_t epoch)` → `detect_result_t`. Inserts/updates a KNOWN row; returns
  `DETECT_KNOWN` on a first hit.
- Eviction sacrifices KNOWN rows before confirmed FOLLOWER rows.
- Bump the NVS record format version (old records load as FOLLOWER).

`radar_wire.h` (shared) + `esp_now_link.c` (`espnow_status_from_webui`):
- Add `kind`/`class_id`/`category`/`confidence` to the per-threat wire element; copy them through.
  Because KNOWN hits share the detect table, `webui_gather_status`/`detect_threat_at` already carry
  them into the snapshot with near-zero extra wiring.

## Data Flow

1. **Author → custody:** seed signatures compiled into firmware. Vigil boots, loads the on-card DB;
   if absent or `content_version` < seed, it seals the seed to `/sdcard/simulacra/threat_sig.db`.
2. **Distribute:** Vigil broadcasts `SIG_SYNC` chunks (version-tagged) every ~60 s.
3. **Adopt:** a decoy reassembles a full `content_version` ≥ its current, re-gates each record, and
   swaps its RAM DB wholesale (idempotent; lost chunks retry next cycle). A decoy with no Vigil keeps
   its seed DB.
4. **Match:** on each observed advert, the decoy runs `sig_match` against the RAM DB.
5. **Record:** a hit calls `detect_note_known` → a KNOWN row in the shared threat table.
6. **Surface:** the row flows to Vigil in the existing status frame; Vigil labels it
   `AirTag (likely)` / `Tile (possible)` and gives KNOWN blips a distinct radar color.

## Match Semantics & Privacy

- The matcher compares only **stable** bytes (company id, type/flags) via the mask; rotating key
  bytes are masked out. A rotating AirTag therefore matches every advert without us tracking *which*
  AirTag. A hit is "an AirTag is present," never an identity.
- Dedup/reporting keys on the hashed current MAC (like `detect`), best-effort. Because RPAs rotate, a
  single physical tracker may reappear as new rows over time — acceptable for slice 1; cross-rotation
  correlation is deliberately *not* attempted (privacy + it's the escalation feature's job).
- `confidence` drives honest UI wording. The design does not claim certainty.

## Error / Edge Handling

- **No SD on Vigil:** RAM-only custody; it still broadcasts the seed-derived DB. (Mirrors learn.db.)
- **Corrupt/foreign card DB:** `sig_db_open` fails → Vigil falls back to the compiled seed and
  reseals. (Mirrors learn.db.)
- **Malformed pushed signature:** `sig_regate` rejects it; a partial/failed version never swaps in.
- **Oversized card DB (future archive):** rejected against `SIG_DB_CAP`-sized buffers, rebuilt from
  seed — same guard as learn.db's RAM-cap check.
- **Matcher bounds:** every masked compare validates `pat_off + pat_len` against the actual AD
  payload length; a short advert simply doesn't match.
- **Table pressure:** KNOWN rows evict before FOLLOWER rows, so a crowd of trackers can't bury a
  confirmed behavioral follower.

## Testing

Pure units, covered in `main/churn_selftest.c` (on-target PASS/FAIL over serial, no radio/screen):
- **Matcher:** crafted AirTag/SmartTag/Tile advert → correct class hit; the same advert with rotating
  bytes flipped → still hits (mask works); a near-miss (one must-match byte changed) → no hit; a short
  advert at a big `pat_off` → no hit, no OOB.
- **Re-gate:** out-of-range `pat_len`/`pat_off`/`category`/`confidence` → rejected.
- **DB seal/open:** round-trip (`sig_db_seal` → `sig_db_open`) recovers records; a corrupted byte →
  open fails; wrong key → open fails.
- **Wire:** `sig_wire` pack → unpack round-trip; reassembly of a multi-chunk version; a dropped chunk
  → no swap.
- **detect integration:** `detect_note_known` inserts a KNOWN row and returns `DETECT_KNOWN`; a KNOWN
  row is evicted before a FOLLOWER under table pressure.

Two-board bench (both wired to the dev PC; serial-verifiable without the CYD screen):
- Vigil pushes DB → decoy log shows `sig: adopted DB v<N> (<count> sigs)`.
- A crafted advert (or a real AirTag brought near the decoy) → decoy log `sig: KNOWN airtag (likely)`
  and the hit appears in the CYD's status-rx / DETAIL surface.

## Seed Signatures (slice 1)

Chosen for well-documented, RF-visible BLE signatures with low false-positive risk. Exact
pattern/mask/offset bytes are finalized during implementation against captured/reference adverts:
- **AirTag / Find My** — Apple company `0x004C`, offline-finding manufacturer-data type byte matched,
  rotating key bytes masked out; addr type RPA/NRPA.
- **Samsung SmartTag** — company `0x0075`, manufacturer-data pattern.
- **Tile** — service-UUID / service-data pattern.

Each carries a `confidence` tier. The set is designed to grow; surveillance/LE classes arrive in a
later slice via the `category` field.

## Future Slices (not this spec)

- Wi-Fi fingerprints (Flock-type cameras, device-name/OUI patterns).
- Surveillance/LE BLE classes (Axon, dashcams).
- Cross-rotation persistence + escalation (`threat-history-escalation`).
- Web-UI / OTA signature editing.
- New node types for sub-GHz (TPMS) and NFC/RFID coverage.
