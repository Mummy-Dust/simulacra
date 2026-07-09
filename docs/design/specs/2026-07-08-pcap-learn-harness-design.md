# pcap → Learn Harness (self-learning v2 prep) — Design Spec

**Date:** 2026-07-08
**Status:** Approved (design), pending implementation
**Related:** self-learning templates (Phase 1/2). Uses the real `learn_strip` / `learn_shape_hash`
/ `learn_merge` / `law3_forbidden` pipeline unchanged.

## Goal

Validate the on-device self-learning pipeline against a **real-world** BLE capture (a drive-time
scan), and produce a **seed library** the fleet can boot with. Two deliverables:

1. A **validation report** proving the `strip → Law-3 → shape_hash → dedup` pipeline behaves
   correctly on messy real adverts (not just bench devices), and that it is **structure-only**
   (no real device identities survive).
2. A **plaintext seed** (`learn.seed`) that a Vigil imports at boot — re-gated on-device — into its
   sealed `learn.db`, so decoys can start with a realistic harvested library.

## Background

- **Capture:** `75af4aa2-new.pcap`, ~7 MB, libpcap DLT 256 (`LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR`):
  each record = 10-byte pseudo-header, 4-byte Access Address, advertising PDU (2-byte header:
  `type` low nibble + length, then payload). Advertising AA = `0x8E89BED6`. Kept **local and
  uncommitted** (uncleaned real-world capture of bystanders' devices).
- **Pipeline (real firmware C, reused as-is):**
  - `bool learn_strip(const uint8_t *ad, uint8_t len, uint16_t company, learned_template_t *out)`
    — parses an AD payload into an identity-stripped skeleton + `rand_mask`; rejects via
    `law3_forbidden`. Self-contained (only `law3` + `fmt_family_t` enum constants; no `esp_random`
    in the strip path).
  - `uint32_t learn_shape_hash(const learned_template_t *t)` — FNV over family/company/svc + the
    AD type/length **sequence** (never value bytes).
  - `bool learn_merge(store, *count, cap, rec, sweep)` — idempotent dedup/reinforce by shape_hash.
  - `bool learn_regate(const learned_template_t *rec)` — budget + Law-3 + shape_hash recompute
    (used on import: never trust a seed).
  - `learned_template_t` is a 55-byte PACKED POD (`ad[31], ad_len, name_off, name_len, rand_mask,
    company_id, svc_uuid, family, itvl_min_ms, itvl_max_ms, shape_hash, reinforce_count,
    last_seen_sweep`).

## Architecture

### Host tool — `tools/pcap_learn/` (no ESP-IDF, no hardware)

```
tools/pcap_learn/
  parse_pcap.py     # pcap -> intermediate advert list
  harness.c         # real pipeline + report + seed emit
  host_stubs/       # tiny shims so firmware C compiles on host
    esp_random.h    # esp_random()->rand, esp_fill_random
    esp_log.h       # ESP_LOG* -> no-op
    nvs.h           # esp_err_t + no-op nvs_* (so learn.c's save/load compile)
    sdkconfig.h     # empty
    templates.h     # ONLY fmt_family_t with the REAL enum values (device-accurate)
  Makefile          # gcc harness.c + real law3.c/learn_wire.c/learn.c
  README.md
```

**`parse_pcap.py`** — pure-stdlib Python (no scapy/tshark). Walks records, skips the 10-byte phdr,
checks the AA, and for advertising PDUs that carry an AD payload — `ADV_IND` (0), `ADV_NONCONN_IND`
(2), `ADV_SCAN_IND` (6), `SCAN_RSP` (4) — extracts `AdvA` and the AD bytes. Skips `SCAN_REQ` (3),
`ADV_DIRECT_IND` (1), `ADV_EXT_IND` (7) (no legacy AD payload). Derives `company_id` from any
`0xFF` manufacturer AD (first two bytes, little-endian) else 0. Emits NDJSON lines
`{"company": <int>, "ad": "<hex>"}` to stdout / a file. Reports per-PDU-type counts to stderr.

**`harness.c`** — reads the NDJSON, and for each advert:
```
learned_template_t rec; memset(&rec,0,sizeof rec);
if (!learn_strip(ad, len, company, &rec)) { rejected++; classify_reason(...); continue; }
rec.shape_hash = learn_shape_hash(&rec);
learn_merge(store, &count, CAP, &rec, sweep++ /* monotonic */);
```
`CAP` sized generously for offline (e.g. 4096) so nothing is evicted during analysis. After the run
it prints the report and writes `learn.seed`.

**Report (stdout):**
- adverts in; parsed ok; rejected (with breakdown: Law-3 forbidden — Apple-popup vs Fast-Pair vs
  other — / malformed / over-budget).
- unique shapes after dedup; distribution of `reinforce_count` (how many real devices collapsed
  into each shape); top-N shapes (family, company, svc, ad_len, reinforce_count).
- **structure-only audit:** for every emitted record, assert that every AD value byte that is NOT a
  preserved structural field (flags/uuid/txpower/appearance/company-id/iBeacon-prefix/svc-uuid) is
  covered by `rand_mask` OR lies in the name region (`name_off..name_off+name_len`). Any identity
  byte left un-masked and un-named is a **FAIL** (the exact property the whole design rests on).

**Seed file `learn.seed`:** `struct { uint32 magic=0x4C534431 "LSD1"; uint16 version=1; uint16 count; }`
then `count` PACKED `learned_template_t` records (top-N by reinforce_count, N configurable). Plaintext
by design — sealing happens on-device.

**Build:** `Makefile` compiles `harness.c` with `-I host_stubs -I ../../components/simulacra_radar
-I ../../main`, and the real `law3.c`, `learn_wire.c`, `learn.c`. Host cc (gcc/clang/mingw). Runs
anywhere; no toolchain beyond a C compiler + Python 3.

### Firmware — Vigil boot-time seed import (`cyd/main/cyd_main.c`)

At Vigil boot, after `learn_db_load()`: if `/sdcard/simulacra/learn.seed` exists, read it, validate
magic/version/count, and for each record run `learn_regate(rec)` (reject any that fail — never trust
a seed) then `learn_merge_wire()` into the RAM library; mark the library dirty so the normal debounced
sealed `learn_db` save persists it; then rename `learn.seed → learn.seed.done` (one-shot). Log
`seed: imported X/Y records -> lib N`. Gated so it is a no-op when the file is absent. This is the
only firmware change; it reuses the existing re-gate + merge + sealed-save paths.

## Testing

- **Host (headless, now):** run `parse_pcap.py` on the real capture → run `harness` → the report
  must show a plausible parse (thousands of adverts, a sane unique-shape count), **zero** structure-only
  audit failures, and a non-empty `learn.seed`. Add a tiny self-check in the harness: re-`learn_regate`
  every emitted seed record and assert all pass (the device will re-gate them identically).
- **Firmware (compile now, SD-test later):** compile-verify the Vigil build with the import path.
  Bench test when hardware is available: drop `learn.seed` on the card → boot Vigil → `seed: imported
  …` → `learndb: saved …` → reboot → library present; `learn.seed.done` left behind.

## Scope

**In:** pcap parse (legacy advertising PDUs), real-pipeline replay, validation report with
structure-only audit, plaintext seed emit, Vigil boot import (re-gated).

**Out (future):** extended-advertising (`ADV_EXT_IND`/AUX) parsing; per-vendor analytics dashboards;
decoy-side seed import (Vigil-only for now); automatically committing any capture-derived data to the
repo (the pcap and any derived seed stay local until reviewed for privacy).

## Privacy / safety

- **Structure-only is the invariant.** The report's audit is a gate, not a nicety: if any real
  identity byte survives stripping, that is a bug to fix before any seed is used.
- The capture and any generated `learn.seed` are **local, uncommitted** artifacts. Only the tool
  source (parser + harness + stubs + Makefile) is committed.
- Import re-gates on-device, so even a hand-edited seed cannot inject a Law-3-forbidden shape.
