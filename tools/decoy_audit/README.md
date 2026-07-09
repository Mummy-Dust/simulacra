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

## One-step (Windows/PowerShell)

`run.ps1` wraps everything — it builds `synth_dump` if missing, then runs all three
steps (handling the ASCII-redirect gotcha for you):

```powershell
.\run.ps1                                   # audit the default bench capture
.\run.ps1 -Capture ..\..\private\x.pcap -Count 512
.\run.ps1 -Gate 0.4                         # exit 1 if headline > 0.4 (CI gate)
.\run.ps1 -Rebuild                          # force a fresh build first
```

Params: `-Capture`, `-Seed`, `-Count`, `-OutDir`, `-Gate`, `-Rebuild`. Needs `cl`
on PATH (a "Developer PowerShell for VS", or run vcvars first). The manual steps
below are the portable/CI form.

## Run the pipeline (manual / portable)

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
vendor_histogram               0.1994 logic
address_type_mix               0.0034 logic
interval_distribution          0.0019 logic
HEADLINE (max) 0.1994  worst tell: vendor_histogram
```

Reading it — two structural tells have been fixed and the honest headline is down
from 0.38 to 0.20:
- **`address_type_mix` (0.003)** — was 0.30 (decoys were 100% static-random, never an
  RPA). `roster.c` now presents a realistic static/RPA/NRPA blend (~52/36/12) via
  `make_random_addr_mixed`; all three are *random* address subtypes, so no real MAC is
  exposed. Verified on the C5: the controller accepts all subtypes (no `set_addr`
  rejections in `churn_adv`).
- **`interval_distribution` (0.002)** — was 0.38. `generate.c` samples
  `other_itvl_bins` for the no-mfg / diversified mass instead of a fast 100–300 ms
  fallback (`other_itvl_bins` had been dead code), so the decoy cadence matches the
  real crowd's heavy-tailed intervals.
- **`vendor_histogram` (0.20)** is now the top tell — device-weighted and honest (see
  the note below). It reflects the decoys carrying template vendors not present in this
  specific environment; largely environment-shaped rather than a structural defect.

> **Why the vendor histogram is device-weighted.** Co-travel correlation tracks
> distinct entities, not advert volume. An earlier advert-weighted metric scored
> `vendor_histogram` at 0.34 — but that was mostly an artifact: one chatty beacon
> emitted 17% of the whole capture, and the histogram was built from only the ~8% of
> devices that expose a `0xFF` company id. Weighting by distinct device and bucketing
> all no-mfg devices/decoys into an explicit `none` category dropped it to 0.16 and
> unmasked the interval tell above.

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
