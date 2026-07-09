# pcap → learn harness

Offline tool that replays a real BLE capture through the **actual firmware**
self-learning pipeline (`learn_strip → learn_shape_hash → learn_merge`, plus
`law3_forbidden`), to (1) **validate** that the pipeline is structure-only on
real-world adverts and (2) emit a **seed library** the Vigil can import.

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
