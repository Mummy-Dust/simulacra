# pcap tools (learn harness + signature scan)

Two offline tools that replay a real BLE capture (parsed once by `parse_pcap.py`)
through **actual firmware code**:

- **`harness`** — the self-learning pipeline (`learn_strip → learn_shape_hash →
  learn_merge`, `law3_forbidden`): validates it is structure-only on real adverts
  and emits a **seed library** the Vigil can import.
- **`sig_scan`** — the tracker/surveillance matcher (`sig_match` + seeded
  `threat_sig` DB): validates/refines the signatures against real data and does
  **per-address dwell analysis** (how long each device stayed near you).

Nothing here runs on hardware; it compiles the real firmware C against small shims
in `host_stubs/`.

## Signature scan (`sig_scan`)

```sh
python parse_pcap.py capture.pcap > adverts.ndjson
make sig_scan
./sig_scan adverts.ndjson
```

Reports tracker hits by class (AirTag/SmartTag/Tile), an AirTag **selectivity**
check (should not fire on ordinary Apple traffic), and **continuous-run analysis**
per matched device: sightings are grouped into encounters (a silence > 60s starts a
new one), and it reports the **longest continuous run** and the **largest gap**. A
device *following* you shows one long run with many sightings; a fixed tag you merely
**passed twice** shows isolated touches with a big gap and a ~0 run (so it is never
mistaken for co-travel). Caveats worth knowing: the seed DB currently holds only 3 *unconfirmed*
tracker signatures (no camera/bodycam vectors yet); AirTags use rotating (RPA)
addresses so dwell can't track them across a moving capture, while Tile/SmartTag
use static addresses and **can** be tracked — so a drive surfaces persistence for
the latter but not the former. Distinguishing a follower from a passerby still
wants a stationary/route capture or the live on-device follower detection.

## Self-learning harness (`harness`)

Offline tool that replays a real BLE capture through the **actual firmware**
self-learning pipeline (`learn_strip → learn_shape_hash → learn_merge`, plus
`law3_forbidden`), to (1) **validate** that the pipeline is structure-only on
real-world adverts and (2) emit a **seed library** the Vigil can import.

**Advertising-interval enrichment:** the seed's per-shape interval band is set from
real timing — mirroring the firmware's on-device estimator (gap between consecutive
sightings of the same device, 20–60000 ms), grouped by advertiser address and taken
as the **median** device interval per shape (robust to sniffer artifacts) with a tight
spread. Shapes with too few timed devices fall back to a **family default** (trackers
slow, iBeacons/wearables faster). Without this, seeded shapes have no interval and
decoys advertise at a generic ~100–300 ms — a timing tell. A moving capture yields
thin timing (few sightings/device, RPA rotation), so most shapes use defaults; a
**stationary capture** produces far better estimates.

Nothing here runs on hardware; it compiles the real `main/learn.c`,
`components/simulacra_radar/{law3,learn_wire}.c` against small host shims in
`host_stubs/`.

## Usage

```sh
# 1. Extract advertising AD payloads from the pcap (DLT 256, BLE LL w/ phdr).
python parse_pcap.py path/to/capture.pcap > adverts.ndjson

# 2. Build (needs a C compiler that honors __attribute__((packed)): gcc or clang).
make

# 3. Run: prints the validation report and writes learn.seed.
./harness adverts.ndjson
```

### On a machine with only MSVC

`cl` ignores `__attribute__((packed))`, so the in-memory structs are padded —
the **report/audit are still valid**, and the seed is emitted via explicit
55-byte little-endian serialization (device-correct regardless of host packing).
Compile with a force-included shim that neutralizes the attribute:

```
echo #define __attribute__(x) > portab.h
cl /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h ^
   /Ihost_stubs /I..\..\components\simulacra_radar /I..\..\main ^
   harness.c ..\..\components\simulacra_radar\law3.c ^
   ..\..\components\simulacra_radar\learn_wire.c ..\..\main\learn.c /Fe:harness.exe
```

## Report

- adverts parsed vs rejected (with a Law-3 vs malformed split);
- unique shapes after dedup;
- **structure-only audit**: every AD value byte must be a preserved structural
  field, masked (`rand_mask`), or in the local-name region — any leaked identity
  byte fails the run (exit 2);
- top shapes by reinforce count;
- retained-name bytes (kept by `learn_strip` for render-time replacement; the
  seed emitter **scrubs** them so no bystander device names leave in the seed).

## Seeding a Vigil

`learn.seed` header is `magic "LSD1" (u32 LE) | version (u16) | count (u16)`
followed by `count` packed 55-byte `learned_template_t` records. Copy it to
`/sdcard/simulacra/learn.seed`; the Vigil imports it at boot — **re-gating every
record** (`learn_regate`: budget + Law-3 + shape_hash recompute — a seed is never
trusted) — merges into the library, saves the sealed `learn.db`, and renames the
file to `learn.seed.done` (one-shot).

## Privacy

The `.pcap` and any generated `learn.seed` are **local, uncommitted** (`.gitignore`).
A drive-time capture contains bystanders' devices; only stripped, name-scrubbed,
structure-only shapes are ever meant to leave this tool, and the device re-gates
again on import.
