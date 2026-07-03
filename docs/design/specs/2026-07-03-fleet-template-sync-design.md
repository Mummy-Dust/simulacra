# Fleet Template Sync — Vigil SD Librarian (Phase 2) — Design

**Date:** 2026-07-03
**Status:** Design approved, ready for implementation plan
**Depends on:** Phase 1 (`2026-07-03-self-learning-templates-design.md`) — the
`learn` module, `learned_template_t`, and `shape_hash`.

## Summary

Phase 1 gives each decoy a local, self-learning template library. Phase 2 makes
that library a **fleet asset**: Vigil (the CYD display node) becomes the fleet's
**template librarian** — decoys push newly-learned shapes to it over the existing
encrypted ESP-NOW link, Vigil merges them into a large SD-card-backed library and
redistributes the best subset back, so every decoy churns from the *combined*
crowd two or more nodes have seen. The library survives reflashes (SD, not NVS)
and effectively loses its size cap.

## Goals

- Aggregate learned shapes from multiple decoys into one SD-backed library on
  Vigil that persists across reflashes.
- Redistribute the merged library so a shape learned on one decoy enriches the
  crowd on another.
- Keep every node fully functional standalone (Vigil optional).
- Never trust a received template: re-gate on every hop.
- Encrypt the library at rest on the removable card.

## Non-goals

- Vigil never *advertises* templates — the CYD is a classic ESP32 with no
  extended advertising. Vigil is storage + distribution only.
- No settings/command control over the link (that is the separate Vigil
  remote-control idea). Phase 2 ships *data* (templates), re-gated on receipt.
- No manifest-negotiated delta protocol — we use delta-up / full-down with
  idempotent merge (simpler, loss-tolerant).

## Roles & topology

- **Ward / Shade (decoys)** — producers + consumers. Harvest (Phase 1), push new
  shapes up, pull the merged library down, and are the only nodes that advertise.
- **Vigil (CYD)** — librarian only. Aggregates, stores on SD, merges,
  redistributes. Never advertises.

```
Ward ──push new/reinforced learned templates──►  Vigil  ◄──push── Shade
                                                   │
                                          merge + dedup by shape_hash
                                          write /sdcard/simulacra/learn.db (encrypted)
                                                   │
Ward ◄──broadcast top-N merged library────────────┴───────────────► Shade
      re-gate + merge into local RAM store  →  generation
```

## Load-bearing principles

1. **Graceful degradation.** Sync is purely additive. Every decoy keeps its
   Phase 1 local NVS store and works standalone. If Vigil is off / absent / out
   of range, or its SD card is missing, decoys still harvest and churn. Vigil
   enriches the crowd; it is never required. Vigil with no SD still runs as a
   pure display.
2. **Never trust a received template.** Every template arriving over the link —
   at Vigil from a decoy, or at a decoy from Vigil — is re-run through the Phase 1
   strip + Law-3 + budget gate before it is stored or emitted. The link is
   AES-GCM + replay-protected so outsiders cannot inject; re-gating means even a
   compromised or buggy key-holder cannot push a pop-up subtype or oversized junk
   into the crowd. (Same "never trust the wire" posture as the CYD `threat_count`
   clamp.)

## Wire protocol

Extends the existing `radar_wire` framing — same
`[magic|ver|type|nonce(12)|ct|tag(16)]`, AES-256-GCM, replay-protected. New
`type` values beside `REQUEST=1` / `STATUS=2`:

- `RADAR_TYPE_LEARN_OFFER` — decoy → Vigil, carries a chunk of learned templates.
- `RADAR_TYPE_LEARN_SYNC` — Vigil → all, carries a chunk of the merged library.

**Frame budget:** ESP-NOW payload is 250 B; wire overhead ~32 B leaves ~218 B →
~3 `learned_template_t` records (56 B) per frame plus a small chunk header
`{ lib_version, chunk_index, chunk_count, record_count }`. A 128-entry library is
~43 frames. All multi-frame transfers are chunked and tolerate broadcast loss.

**Idempotence is the reliability model.** Merge dedups by `shape_hash`, so
re-receiving a record is harmless. The protocol needs to be *eventually
complete*, not perfectly delivered.

## Sync model — delta-up, full-down

- **Decoy uplink (delta):** promoting/reinforcing a shape sets a `dirty` bit
  (a RAM-only local flag added for this phase — **not** serialized to NVS/SD and
  **not** part of the on-wire or on-disk record, which stays 56 B). On the ch1
  listen window the
  decoy already uses to hear Vigil, if any templates are dirty it sends
  `LEARN_OFFER` chunks of just those. A `dirty` bit clears only when that
  `shape_hash` returns in a Vigil `LEARN_SYNC` (self-confirming — a lost offer
  stays dirty and re-offers). No dedicated ack frame.
- **Vigil downlink (full):** Vigil broadcasts the top-N of its merged library as
  `LEARN_SYNC` chunks on a background timer (radio-calm), and again shortly after
  a merge changes something.
- **No feedback loop:** only *locally harvested* shapes set `dirty`; templates
  received from Vigil are merged but never marked dirty, so they are never echoed
  back up.

## Merge semantics (idempotent, keyed by `shape_hash`)

- **reinforce_count:** saturating add (cap `uint16` max) — a shape seen across
  many nodes legitimately weights higher.
- **interval band:** union — `min(mins)`, `max(maxs)`, clamped to sane bounds.
- **last_seen:** `max` — most recent sighting wins for age-out.
- **family / company_id / svc_uuid:** identical by definition (inputs to
  `shape_hash`), no conflict.

Age-out stays per-node: Vigil ages by wall-clock over a long window (days);
decoys age by sweep, and a received `LEARN_SYNC` refreshes `last_seen` so
actively-present shapes never decay. A shape gone fleet-wide eventually falls out
everywhere.

## SD storage on Vigil

- **Mount:** microSD over SPI via `esp_vfs_fat_sdspi_mount`.
- **File:** `/sdcard/simulacra/learn.db` — `header (magic, version, nonce, tag,
  count)` + array of on-disk template records. Small, so merges rewrite it
  **atomically** (write `.tmp`, `rename`).
- **At-rest encryption (required):** the blob body is AES-256-GCM encrypted with
  a key derived from the fleet PSK via HKDF-SHA256 under a dedicated context
  label (distinct from the ESP-NOW session key). A random nonce + tag live in the
  header. On read, decrypt-and-verify; a failed tag (corrupt / tampered / foreign
  card) is treated as "no library" and rebuilt from the next sync. No plaintext
  `.csv` export — an inspection dump would defeat the at-rest guarantee, so it is
  a deliberate authenticated decrypt-to-view action only.
- **Graceful:** SD absent / unmountable → Vigil runs as a pure display, no
  librarian function.

**SD superset vs decoy working set.** SD removes the cap for Vigil (thousands of
shapes). Each decoy still has a bounded RAM store (Ward 128 / Shade 64). So
`LEARN_SYNC` sends the **top-N by `reinforce_count`** that fit the smallest decoy
cap — the fleet's most-established shapes propagate while SD keeps the long tail.
Vigil is the deep archive; decoys carry the best working subset.

**Hardware caveat (validate early):** the CYD's SD card and ILI9341 display share
the SPI subsystem. Workable, but SD I/O must cooperate with display DMA — flagged
as an integration risk to confirm on the bench, not a blocker.

## Vigil UI

One new touch page in the existing page-cycle: library size, last merge/sync
time, SD mount status — so the librarian role is visible. Low priority.

## Security summary

- Injection: blocked by AES-GCM + replay protection (existing link).
- Malicious/buggy key-holder pushing junk: blocked by re-gating every received
  template at both ends.
- Card theft: blocked by at-rest encryption; and even decrypted, the data is
  stripped shapes (no identities) by Phase 1 construction.
- Relates to fleet key-provisioning (a leaked PSK now also exposes the library) —
  tracked as a future hardening item, not in scope here.

## Error handling

- Dropped sync frames: harmless (idempotent merge; re-sent next cycle).
- Partial library received: usable immediately; completes over subsequent cycles.
- SD write failure: keep serving the in-RAM working set; retry on next merge.
- Corrupt / foreign `learn.db`: rebuilt from sync, not fatal.
- Vigil offline: decoys operate standalone on their Phase 1 stores.

## Testing

Host-runnable (bulk, no radio):

- `LEARN_OFFER` / `LEARN_SYNC` chunk encode↔decode round-trip.
- Reassembly **completes despite dropped frames** (idempotent-completeness).
- Merge: dedup, saturating reinforce, interval union, `last_seen = max`.
- **Crux security test:** a received template with a pop-up subtype / oversized /
  garbled payload is **rejected by the re-gate at both Vigil and the decoy**.
- Dirty-bit: set on promote, clears only on downlink confirmation; lost offer
  stays dirty.
- Top-N-by-reinforce selection fits the smallest decoy cap.
- SD: encrypt/decrypt round-trip, tamper → rebuild, atomic `.tmp`-rename write,
  graceful no-SD path.

On-target 3-board soak: Ward + Shade harvest → Vigil aggregates to SD → library
propagates back; verify a shape learned on **Ward** later appears in **Shade's**
roster via Vigil. Pull the SD card mid-run (Vigil degrades to display); corrupt
`learn.db` (rebuilds).

## Configuration

| Symbol | Default | Meaning |
|---|---|---|
| `SIMULACRA_FLEET_SYNC` | on when ESP-NOW enabled | build gate for Phase 2 |
| `LEARN_SYNC_PERIOD_MS` | background, radio-calm | Vigil downlink cadence |
| `LEARN_SYNC_TOP_N` | smallest decoy cap | records broadcast per full-down |
| `SD_MOUNT_POINT` | `/sdcard` | FAT mount |
| `LEARN_DB_PATH` | `/sdcard/simulacra/learn.db` | encrypted library file |
