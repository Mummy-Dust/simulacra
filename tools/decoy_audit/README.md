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
  `scorecard.py` ranks them and reports the max as the headline. Tells: address-type mix,
  interval distribution, vendor histogram, and **AD structure** (the ordered AD element-type
  signature each device advertises, e.g. `01,03,16` — device-weighted, what a sniffer reads
  off the raw advert). Also scored: **presence/lifespan** (per-address on-air duration) and a
  modeled **physical (RSSI)** tell (see below).

## Build `synth_dump`

MSVC (this host has no gcc/clang) — from `tools/decoy_audit/`:

```
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h ^
   /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar ^
   synth_dump.c ble_hs_adv.c roster_stub.c ^
   ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c ^
   ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ^
   ..\..\main\uniq_id.c ..\..\main\phantom.c ..\..\main\probe_agents.c ..\..\main\probe_frame.c ^
   ..\..\main\fleet_pop.c ..\..\main\fleet.c ^
   /Fe:synth_dump.exe
```

gcc/clang CI: `make` (the `Makefile` uses the same sources and include paths).

Notes on the host shim:
- `portab.h` is force-included (`/FIportab.h`); it neutralizes GCC `__attribute__`
  and supplies a portable `__builtin_popcount` that MSVC lacks.
- The real `main/` headers win for quoted includes, so `churn.h`/`learn.h` are the
  real ones; only the leaf host headers are stubbed (`host_stubs/`: `esp_random.h`,
  `esp_log.h`, `sdkconfig.h`, `nvs.h`, `host/ble_hs.h`, `host/ble_uuid.h`). The audit
  builds the **real** `main/learn.c` (+ `law3.c`, `learn_wire.c`) so it can measure
  self-learned shapes; `roster_stub.c` supplies the one leftover `rf_model_load_nvs`.

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

### Baseline (`private/long.pcap`: 193,363 adverts / 689 advertisers)

```
DISCRIMINATOR            SEPARABILITY VISIBILITY
ad_structure                   0.1526 logic     <- added + closed 2026-07-13
interval_distribution          0.0026 logic
address_type_mix               0.0019 logic
vendor_histogram               0.0015 logic
HEADLINE (max) 0.1526  worst tell: ad_structure
```

**AD-structure: found at 0.88, closed to 0.15 (2026-07-13).** Adding the tell exposed a gap the
three distributional tells were blind to — the decoys advertised beacon-rich payloads (`01,03,16`
service-data **86%**) while real ambient BLE is terse (`01` flags-only **53%**, `01,03` flags+uuid
**21%**, `01,ff` flags+mfg **11%**). Fix: two minimal-advertiser families (`FMT_FLAGS_ONLY` → `01`,
`FMT_SVC_UUID16` → `01,03`) and a reweighted no-mfg structural mix in `generate.c`
(`pick_no_mfg_template`: ~62% flags-only, ~24% flags+uuid16, ~14% service-data beacon). Decoys now
match the real crowd on the two biggest signatures almost exactly (`01` 0.527 vs 0.527, `01,03` 0.20
vs 0.21). The residual 0.15 is the deliberate tracker/beacon persona (`01,03,16` ~14%, ~0 in *this*
capture but present in real environments) plus real signatures the decoys don't emit by design
(empty adverts 9%, flags+name 5%, flags+TX-power 1%). Driving it lower would over-fit one capture's
beacon count and delete the beacon persona. NOTE: `long.pcap` is real-ambient+decoy *mixed*, a
relative measure; the structural direction is robust regardless.

The three distributional tells remain closed; the honest headline history is 0.38 → 0.004
(distributional) → 0.88 (AD structure found) → 0.15 (AD structure closed):
- **`address_type_mix` (0.001)** — was 0.30 (decoys were 100% static-random, never an
  RPA). `roster.c` now presents a realistic static/RPA/NRPA blend (~52/36/12) via
  `make_random_addr_mixed`; all three are *random* address subtypes, so no real MAC is
  exposed. Verified on the C5: the controller accepts all subtypes (no `set_addr`
  rejections in `churn_adv`).
- **`interval_distribution` (0.004)** — was 0.38. `generate.c` samples
  `other_itvl_bins` for the no-mfg / diversified mass instead of a fast 100–300 ms
  fallback (`other_itvl_bins` had been dead code), so the decoy cadence matches the
  real crowd's heavy-tailed intervals.
- **`vendor_histogram` (0.002)** — was 0.20. Two fixes: the audit now parses each
  decoy's company id from the **serialized payload** (`company_onair`, the same walk
  `capture_profile._company` runs on the real side) instead of trusting the identity's
  label — so a Tile reads as no-mfg (service-data only) and an iBeacon reads as Apple
  `0x004C`, exactly as an over-the-air sniffer would. And `generate.c` now keeps the
  no-mfg (OTHER) mass on the service-data families (eddystone/tile, varied within them)
  instead of leaking it into manufacturer/iBeacon templates, so the decoy no-mfg share
  matches the real crowd's ~89%.

> **Why the vendor histogram is device-weighted and on-air-parsed.** Co-travel
> correlation tracks distinct entities, not advert volume, so the histogram is
> DEVICE-weighted (an earlier advert-weighted metric scored 0.34, inflated by one
> chatty beacon emitting 17% of the capture). And because a real adversary parses the
> transmitted bytes, the synthetic side is scored on the payload it actually
> broadcasts, not its internal label — closing a gap where service-data decoys were
> mislabeled with a manufacturer id.

## Regression gate

`scorecard.py --gate <x>` exits `1` when `headline > x`, `0` otherwise. Wire it
into CI with a committed model-seed fixture (not a bystander capture) to track the
headline dropping as realism work lands.

## Scoring vs. calibration

v1 has only **distributional** discriminators (address-type mix, interval, vendor),
for which Jensen–Shannon divergence is the natural separability measure. The
FPR-pinned **per-device** calibration described in the design spec arrives with the
physical-only slice (per-advert "looks synthetic" tells), where that framing applies.

## Learned-shape audit

The audit builds the **real** `main/learn.c`, so it can score the decoys' *self-learned*
shapes, not just the built-in templates. Enable it by passing a `learn.seed` as the 4th
`synth_dump` arg — that loads the learned library into the real store, so `generate_roster`
reproduces those shapes via `learn_render` (exactly as the firmware does). Absent, the audit
is template-only and byte-identical to before.

Produce a seed from a real capture with the existing `tools/pcap_learn` harness (it replays the
capture through the actual firmware pipeline `learn_strip → learn_shape_hash → learn_merge` and
runs a structure-only leak audit):

```
# from tools/pcap_learn/  (cmd redirection: PowerShell '>' would UTF-16 the NDJSON)
cmd /c "python parse_pcap.py ..\..\private\long.pcap > ..\..\private\adverts.ndjson"
harness ..\..\private\adverts.ndjson          # writes learn.seed (CWD); prints the leak audit
# then, from tools/decoy_audit/
synth_dump 1 256 ..\..\private\model.seed ..\..\private\learn.seed | ... | scorecard.py ...
```

**Result (`private/long.pcap`):** the harness stripped 182k adverts to **4** learned shapes
(iBeacon/Samsung mfg; 98.6% of the terse-majority adverts have nothing distinctive to learn) with
a **PASS structure audit (0 leaked identity bytes)**. Enabling those shapes moved the headline
**0.15 → 0.12** — self-learning introduces *no* new tell and marginally *improves* realism, because
reproducing a real observed shape is at least as faithful as a hand-built template. The strip→render
round-trip is validated: faithful and identity-safe.

## Presence-duration tell

The **presence_duration** discriminator scores how long each address stays observable
(per-address, the passive-sniffer projection of rotation + lifetime) against the real crowd's
`presence_ms_bins`. It needs the temporal `synth_dump --devices` run, so pass it with
`scorecard.py --devices <run>` (run.ps1 does this automatically). Bins: <1/5/15/30/60/120/>120 min.

> **The metric was wrong before.** `rotation_audit` measured presence as the born→last-rotate
> span, scoring a non-rotating STATIC address (~half the fleet) as **0** — a false 0.78 alarm. The
> correct per-address presence is the gap until the same slot's next address replaces it; the honest
> tell is ~**0.13**. To close the real residual — real ambient has a ~15% long-lived static tail the
> churning fleet lacked — `BLE_ROLE_PERSISTENT` gives a slice (~28%) of static devices a 4–12 h
> lifetime (only static can persist; RPA/NRPA rotate regardless). Presence JSD **0.13 → 0.084**.
> The remainder is an honest ceiling: presence is address-weighted, and a fleet that must churn to
> defeat tracking necessarily emits many short-lived addresses.

## Deferred slices

- **AD-structure discriminator** — DONE (2026-07-13). Scored on the AD element-type
  *sequence*, not exact bytes: `ble_hs_adv.c` emits fields in NimBLE's canonical order and
  the templates set only those fields (deliberately no TX-power AD element, templates.c),
  so the host type-sequence matches the on-air structure faithfully — no byte-accurate
  serializer needed. A byte-value discriminator stays deferred (payloads are largely random
  per-advert, so exact bytes would measure noise, not structure).
- **Learned-shape path** — DONE (2026-07-13). See "Learned-shape audit" below.
- **Presence / lifespan cohort** — DONE (2026-07-13). See "Presence-duration tell" above.
- **RSSI / TX-power spread** — still deferred: a physical-layer tell that needs a **decoy-only
  capture** as a clean label source (`long.pcap` carries RSSI but is real+decoy mixed, so signal
  alone can't label which adverts are decoys).

## Privacy

The repo is public. Captures and every parsed intermediate (`profile.json`,
`*.ndjson`, `*.seed`, `card.json`) live under the gitignored `private/`.
`profile.json` retains only aggregate distributions — never device addresses,
names, or raw AD. No absolute paths or usernames belong in any committed file here.

## Dual-radio coverage (cross-protocol personas)

`synth_dump --personas <seed> <n_phantoms> <n_ble> <ticks> <tick_ms>` drives the full bound
system (the phantom registry + probe agents + ble_devices) and emits `W` (Wi-Fi) and `B` (BLE)
events. `tests/test_personas.py` asserts the properties that make a persona a convincing dual-radio
device: **dual-radio coverage** (every Wi-Fi identity has a co-present BLE twin), co-appearance on
the same tick, that persona BLE members are RPA and Law-3-safe (never Apple mfg data), a **realistic
phone shape** (terse flags-only / 16-bit service-UUID list, no accessory manufacturer data),
cross-radio address uniqueness, and a **widened phone interval band** (personas spread across
intervals rather than clustering on one accessory value).
This is the "are our fake phones real dual-radio devices?" axis: a real phone-heavy environment has
near-100% coverage with diverse payloads, so the decoys must too.

## The physical (RSSI) tell — modeled

An ESP32 cannot hear its own injected frames, so decoy RSSI cannot be captured on
the board. The scorecard instead **models** it: single-node worst case, where all
decoys emit from one antenna, so their RSSI spread is only the generator's
per-identity tx-power dither (`{-12,-9,-6,-3,0,+3}` dBm) plus a fixed-position
multipath jitter (σ = 4 dB). That modeled spread (~6.5 dB) is scored, placement-
invariant (median-centered), against a real crowd's RSSI distribution from an
RSSI-bearing capture (Nordic DLT157 or LE-LL-with-PHDR DLT256).

The row is labeled `visibility=modeled` and is **headline-eligible**: the physical
layer is real exposure (an adversary co-located with the decoys sees it). In
practice the per-identity tx-power dither keeps it **modest** — measured ~0.15
against real captures (modeled ~6.6 dB spread vs a real crowd's ~12 dB), comparable
to the well-controlled structural tells, not dominant. Its value is as a regression
gate: drop the dither and the spread collapses, spiking the tell. σ is anchored to a
real over-air decoy capture (the modeled spread sits below the measured 10.1 dB),
not invented. For a real over-air number when a decoy OBSERVE capture is available,
use `analyzers/rssi_audit.py`.

Because the tell scores *shape* against a real reference, the reference matters: a
stationary single-vantage capture has a narrow real RSSI spread of its own, which
*understates* the tell; a spatially diverse capture is the more honest reference.
