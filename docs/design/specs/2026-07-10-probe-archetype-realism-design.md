# Probe-Request Archetype Realism (design spec)

**Status:** Approved (2026-07-10). Follow-on to
[`2026-07-01-m7-wifi-probe-injection-design.md`](2026-07-01-m7-wifi-probe-injection-design.md),
which shipped the injector with a minimal generic IE tail and explicitly left "a generic HT
capabilities IE for modern realism" as optional/future. This spec makes the emitted probe requests
**byte-faithful to real modern phones** so a Wi-Fi sniffer cannot fingerprint them as "too plain."
First (and for now only) target: the **C5 (Ward)**, dual-band.

**Goal:** replace the minimal IE tail (Supported Rates + Extended Rates + DS Param + wildcard SSID)
with **per-archetype, per-band information-element sets** authored byte-for-byte from documented
real probe requests, drawn from a small library of phone archetypes, verified by host unit tests
that assert the built frame equals a checked-in reference fixture.

## Scope decisions (from brainstorming)

- **Realism now; capture-driven enrichment later.** Author archetype IE blobs from documented probe
  structures in this milestone. A later milestone ingests real captures — each becomes a new/updated
  reference blob. The host-test harness is built now to accept them.
- **Small archetype library:** four hand-crafted, high-fidelity archetypes — **iPhone**, **Samsung
  Galaxy**, **Pixel**, **generic Android** — weighted **40 / 25 / 15 / 20** for pool draws. Adjustable.
- **Independent pool.** The Wi-Fi phone archetypes are their own fixed spread; they are **not** linked
  to the board's BLE decoy identities. Rationale: real phones rotate Wi-Fi MAC and BLE address on
  independent schedules so the two are deliberately unlinkable — per-device linkage would itself be a
  tell. (Population-level vendor-mix consistency with BLE is noted as a possible future refinement,
  not in scope.)
- **Fidelity verified by host unit-test fixtures** (mirrors `tools/decoy_audit`): build each
  (archetype, band) frame on the host and assert structure + byte-exact match to a reference.

## Invariants (inherited from M7 — still hard-guarded)

- **Wildcard SSID only — never directed.** Every archetype blob's SSID IE is id `0x00`, **length 0**.
  No real network names, ever. Regression-guarded in the host tests.
- **Randomized locally-administered MACs.** `probe_random_mac` unchanged (LAA bit set, unicast). No
  real OUI; no observed/copied MAC.
- **Non-targeted, non-associating, nothing persisted.** Probe requests only.
- **No identity in IEs.** Vendor-specific IEs carry only capability/type bytes (Apple `00-17-F2`
  device-capability type; Android WPS `00-50-F2` / P2P `00-6F-9A`) — never serials, names, or PNL.
  Verified by inspecting each authored blob and documenting its `// source:` provenance.

## Architecture

### New file split: `probe_frame.c` / `probe_frame.h` (pure, host-testable)

The pure frame builder + archetype data move out of `probe.c` into `probe_frame.c` (deps:
`<string.h>`, `<stdint.h>`, `esp_random.h` only). `probe.c` keeps the ESP-only radio path
(`esp_wifi`, FreeRTOS, logging) and `#include`s `probe_frame.h`. This lets the host test compile a
small pure unit with just the existing `esp_random` stub, instead of stubbing all of `esp_wifi`.

### Archetype library

```c
typedef enum { ARCH_IPHONE, ARCH_GALAXY, ARCH_PIXEL, ARCH_ANDROID, PROBE_ARCH_COUNT } probe_arch_t;

typedef struct {
    const char    *name;
    const uint8_t *tail24; uint16_t tail24_len; int16_t ds_off24; // DS-Param channel byte offset; -1 = band absent
    const uint8_t *tail5;  uint16_t tail5_len;  int16_t ds_off5;
    uint8_t        weight;                                        // fixed independent draw spread
} probe_archetype_t;

const probe_archetype_t *probe_archetype(probe_arch_t a);
size_t                   probe_archetype_count(void);
probe_arch_t             probe_pick_archetype(void);             // weighted draw (uses esp_random)
```

Each `tail` blob is the full IE body (everything after the 24-byte MAC header + seq control),
authored byte-for-byte from a documented probe request for that device/band:

- **2.4 GHz tail:** wildcard SSID (`00 00`), Supported Rates (`0x01`, incl. CCK 1/2/5.5/11),
  Extended Rates (`0x32`), DS Param (`0x03`, channel patched), HT Capabilities (`0x2D`),
  Extended Capabilities (`0x7F`), vendor IEs (`0xDD`).
- **5 GHz tail:** wildcard SSID, Supported Rates (**no CCK** — OFDM 6..54 only), DS Param,
  HT Capabilities (`0x2D`), VHT Capabilities (`0xBF`), HE Capabilities (extension id `0xFF`/`0x23`),
  Extended Capabilities, vendor IEs.

All four archetypes are dual-band (both tails present). `ds_offNN` records the byte offset of the
DS-Param channel field within the tail so the builder can patch it without re-parsing.

### Frame builder

```c
// Build a wildcard probe request for `mac` on `ch`, using archetype `arch`'s per-band IE set.
// band5 selects the 5 GHz tail. Writes <= PROBE_FRAME_MAX bytes. Returns 0 on success,
// non-zero if the archetype lacks the requested band.
int probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                        uint8_t *out, size_t *out_len);
```

Builds the 24-byte MAC header exactly as today (FC `0x0040`, DA/BSSID broadcast, SA = `mac`, seq 0),
copies the selected tail, patches the DS-Param channel byte at `ds_offNN`. **`PROBE_FRAME_MAX`
raised 64 → 256** (HT+VHT+HE+ext-cap+vendor IEs exceed 64).

### Pool + rotation (`probe.c`)

Each fake phone becomes `{ uint8_t mac[6]; probe_arch_t arch; }`. Archetype is drawn (weighted) once
at `probe_pool_init` and **fixed for that MAC's lifetime** — a phone does not change model. When a
MAC retires during rotation, its archetype is redrawn with the fresh MAC. `probe_inject_burst`
computes `band5 = (channel >= 36)` and builds each phone's frame with its archetype + band; an
archetype missing the band is skipped (all four are dual-band, so this is defensive only). A one-time
`ESP_LOGW` reports the realized archetype mix.

## Host test harness: `tools/probe_audit/`

Mirrors `tools/decoy_audit` (Makefile + `run.ps1` + reuse `host_stubs/` for `esp_random`). Compiles
`probe_frame.c`. A `probe_dump.c` host `main` builds every (archetype, band) frame and emits hex;
`tests/test_probe_frame.py` asserts, for each archetype/band:

1. Parses as a valid probe request: FC `0x40 00`, DA and BSSID both broadcast.
2. **SSID IE present and wildcard (id 0, len 0)** — the Law-3 regression guard.
3. Expected IE ids present in the documented order (e.g. `01, 32, 03, 2D, [BF, FF] , 7F, DD…`).
4. DS-Param channel byte equals the requested channel.
5. Total length ≤ `PROBE_FRAME_MAX`.
6. **Byte-for-byte equals the checked-in reference fixture** for that (archetype, band) — the core
   guarantee of the reference-blob approach.

Fixtures live under `tools/probe_audit/fixtures/<arch>_<band>.hex`; they are the same bytes as the
authored blobs and carry a `# source:` provenance line. When real captures arrive, they replace these
fixtures and the tests re-pin to the captured bytes.

## Out of scope (YAGNI)

- **No BLE coupling** (independent pool).
- **No burst-timing/inter-frame realism** — frame *content* is the dominant tell; timing patterning
  is a later phase.
- **No directed/SSID probes** (Law-3: wildcard only).
- **No real-capture ingestion** — deferred; the harness is built to accept captures as fixtures.
- **C6 (Shade)** stays on its existing minimal 2.4-GHz path for now; archetypes are C5-first. (The
  builder is board-agnostic, so extending to the C6's 2.4 tails later is trivial.)

## Success criteria

- `tools/probe_audit` host tests green: IE structure + wildcard SSID + DS channel + byte-exact
  fixtures for all four archetypes × both bands.
- C5 firmware builds (`esp32c5`), flashes, and serial shows probe bursts as today.
- Frames captured off-air decode in Wireshark as the intended archetypes with **no malformed-IE
  warnings** (secondary on-target confirmation; the host fixtures are the authoritative guard).

## Files

- **Create:** `main/probe_frame.h`, `main/probe_frame.c` (archetype data + pure builder);
  `tools/probe_audit/` (`Makefile`, `run.ps1`, `probe_dump.c`, `tests/test_probe_frame.py`,
  `fixtures/*.hex`).
- **Modify:** `main/probe.c` (drop pure builder; per-phone archetype in pool + rotation; band
  selection in burst), `main/probe.h` (`PROBE_FRAME_MAX` → 256; updated builder signature or a thin
  back-compat wrapper), `main/CMakeLists.txt` (add `probe_frame.c`).
