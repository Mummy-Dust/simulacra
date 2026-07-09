# decoy_audit — decoy detectability self-audit

Measures how separable Simulacra's synthetic BLE decoys are from a real ambient
crowd, and turns "are our decoys convincing?" into a ranked scorecard plus a
single regression-gate number. It compiles the **real** firmware generation path
(`main/generate.c` + `templates.c` + `roster.c`) on the host so every synthetic
advert is a known-decoy label for free, then scores each feature's divergence
from the distribution of a real capture. The headline is the **worst (max) tell**
— a real adversary picks their single best discriminator, so the worst tell
defines our exposure.

## The three units

- **`synth_dump`** (C) — runs the real generator against an `rf_model` and dumps a
  labeled decoy population as NDJSON (`addr, atype, company, itvl_ms, tx, arch, plen`).
  The self-learning path is disabled (`learn_stub.c` → `learn_count()==0`) so the
  audit measures the built-in-template generator; `srand(seed)` makes it deterministic.
- **`capture_profile.py`** — parses a BLE capture into an aggregate distribution
  profile (`profile.json`) and a matched `model.seed`. Handles both DLT256 and
  Nordic DLT157 by scanning each record for the advertising Access Address
  `d6 be 89 8e`; address type comes from the AdvA MSB top-2-bits, **not** the PDU
  TxAdd bit (unreliable in the Nordic framing). Emits only distributions — never
  addresses, names, or AD payloads.
- **`discriminators.py` + `scorecard.py`** — score each tell as the Jensen–Shannon
  divergence (base-2, in `[0,1]`) between the synthetic and real distributions:
  `0` when synth matches real (never fires on a faithful crowd), `1` when disjoint.
  `scorecard.py` ranks them and reports the max as the headline.

## Build `synth_dump`

MSVC (this host has no gcc/clang) — from `tools/decoy_audit/`:

```
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h ^
   /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar ^
   synth_dump.c ble_hs_adv.c learn_stub.c ^
   ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ^
   /Fe:synth_dump.exe
```

gcc/clang CI: `make` (the `Makefile` uses the same sources and include paths).

Notes on the host shim:
- `portab.h` is force-included (`/FIportab.h`); it neutralizes GCC `__attribute__`
  and supplies a portable `__builtin_popcount` that MSVC lacks.
- The real `main/` headers win for quoted includes, so `churn.h`/`learn.h` are the
  real ones; only the leaf host headers are stubbed (`host_stubs/`: `esp_random.h`,
  `esp_log.h`, `sdkconfig.h`, `host/ble_hs.h`, `host/ble_uuid.h`). `learn_stub.c`
  implements the learn API as no-ops and stubs `rf_model_load_nvs` (no NVS on host).

## Run the pipeline

Captures and their intermediates stay under the gitignored `private/`.

```
# 1. real crowd -> profile + model-seed
python capture_profile.py ../../private/long.pcap ../../private/profile.json ../../private/model.seed
# 2. synthetic decoys matched to that crowd
./synth_dump 1 256 ../../private/model.seed > ../../private/synth.ndjson
# 3. scorecard (add --gate <x> to fail CI when the headline regresses above x)
python scorecard.py ../../private/synth.ndjson ../../private/profile.json --json ../../private/card.json
```

> On PowerShell, `>` re-encodes stdout as UTF-16; redirect with
> `| Out-File -Encoding ascii` instead (or run step 2 under `cmd`/bash).
> `scorecard.py` also tolerates a BOM defensively.

### Baseline (2026-07-09, `private/long.pcap`: 193,363 adverts / 689 advertisers)

```
DISCRIMINATOR            SEPARABILITY VISIBILITY
vendor_histogram               0.3401 logic
address_type_mix               0.2997 logic
interval_distribution          0.1367 logic
HEADLINE (max) 0.3401  worst tell: vendor_histogram
```

Reading it: the reference crowd was a near-monoculture (one company id ~95% of
adverts). The generator's diversity floor (`GEN_MAX_VENDOR_PCT`) deliberately caps
any single vendor and redistributes to varied templates — good against co-travel
correlation, but it makes the synthetic vendor histogram diverge from a monoculture
environment, so `vendor_histogram` is the top tell here. `address_type_mix` is the
structural runner-up: decoys are 100% static-random, while the real crowd was
~52% static / 36% RPA / 12% public — decoys never present an RPA.

## Regression gate

`scorecard.py --gate <x>` exits `1` when `headline > x`, `0` otherwise. Wire it
into CI with a committed model-seed fixture (not a bystander capture) to track the
headline dropping as realism work lands.

## Scoring vs. calibration

v1 has only **distributional** discriminators (address-type mix, interval, vendor),
for which Jensen–Shannon divergence is the natural separability measure. The
FPR-pinned **per-device** calibration described in the design spec arrives with the
physical-only slice (per-advert "looks synthetic" tells), where that framing applies.

## Deferred slices

- **Byte-accurate NimBLE serializer** for an AD-structure discriminator. v1's
  `ble_hs_adv.c` is length-correct with canonical-order TLVs; the current
  discriminators never read the serialized bytes, so scores are faithful.
- **Physical-only discriminators** — RSSI/TX spread, lifespan cohort — need a
  decoy-only capture as a clean label source.
- **Learned-shape path** — currently disabled via `learn_stub.c`; enabling it
  (compile the real `main/learn.c` + a seeded library) is a later slice once the
  built-in-template baseline is trusted.

## Privacy

The repo is public. Captures and every parsed intermediate (`profile.json`,
`*.ndjson`, `*.seed`, `card.json`) live under the gitignored `private/`.
`profile.json` retains only aggregate distributions — never device addresses,
names, or raw AD. No absolute paths or usernames belong in any committed file here.
