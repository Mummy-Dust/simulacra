# Learned-Shape Audit Slice — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Extend `tools/decoy_audit` to score the decoys' *self-learned* shapes (not just the built-in templates), so the audit covers Simulacra's flagship self-learning path instead of stubbing it off.

**Architecture:** The existing `tools/pcap_learn/harness` already replays a real capture through the actual firmware learn pipeline (`learn_strip → learn_shape_hash → learn_merge`) and emits a packed `learn.seed`. This slice (1) produces that seed from `private/long.pcap`, (2) teaches `synth_dump` to load a `learn.seed` into the real `learn.c` store so `learn_count()>0` and `generate_roster` reproduces learned shapes via `learn_render`, and (3) runs the audit on the learned decoys. Measures the generator *as the firmware runs it* (learning enabled), validating that the strip→render round-trip preserves realism.

**Tech Stack:** MSVC host build (`cl`), Python 3 stdlib `unittest`, the real firmware C (`learn.c`, `law3.c`, `learn_wire.c`, `generate.c`, `templates.c`, `roster.c`, `ble_devices.c`).

## Global Constraints

- Public repo. `learn.seed`, `adverts.ndjson`, and all capture intermediates stay under gitignored `private/`. No addresses/names/AD payloads or absolute paths in committed files.
- Host is MSVC-only (`cl`); no gcc/clang. Python is stdlib `unittest` only.
- The `learn.seed` binary format is fixed by `tools/pcap_learn/harness.c`: 8-byte header (`uint32 magic 0x4C534431 "LSD1"`, `uint16 ver=1`, `uint16 count`) then `count` × 55-byte little-endian records: `ad[31]`, `ad_len`, `name_off`, `name_len`, `rand_mask(4)`, `company_id(2)`, `svc_uuid(2)`, `family(1)`, `itvl_min_ms(2)`, `itvl_max_ms(2)`, `shape_hash(4)`, `reinforce_count(2)`, `last_seen_sweep(2)`.
- Keep the default (no-learn) audit path byte-identical: the learned path is opt-in via a new 4th `synth_dump` arg, so existing runs and the AD-structure baseline are unchanged.

---

### Task 1: Produce and validate `learn.seed` from the real capture

**Files:** none committed (outputs to `private/`).

**Interfaces:**
- Consumes: `private/long.pcap`, `tools/pcap_learn/{parse_pcap.py,harness.c,Makefile}`.
- Produces: `private/learn.seed` (input to Tasks 2-3).

- [ ] **Step 1: Parse the capture to NDJSON**

Run (from `tools/pcap_learn/`): `python parse_pcap.py ../../private/long.pcap > ../../private/adverts.ndjson`
Expected: a non-empty NDJSON file (one advert per line).

- [ ] **Step 2: Build the harness (MSVC)**

`cl` equivalent of the Makefile (gcc not present). Run from `tools/pcap_learn/`:
```
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /Ihost_stubs /I..\..\components\simulacra_radar /I..\..\main harness.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c ..\..\main\learn.c /Fe:harness.exe
```
Expected: `harness.exe` builds (the Makefile's `-Wno-unused-parameter` maps to nothing on cl; add `/FIportab.h`-style shims only if a packed/attribute error appears — `learn_record.h` already guards `__attribute__` under `_MSC_VER`? verify; if not, add `/Dpacked=` is NOT valid — instead confirm the harness host_stubs compile clean on cl, mirroring decoy_audit).

- [ ] **Step 3: Run the harness -> seed + structure audit**

Run: `.\harness.exe ..\..\private\adverts.ndjson` with the working dir set so `learn.seed` lands in `private/` (harness writes `learn.seed` to CWD — run from `private/` or move the output). 
Expected: report prints `identity leaks: 0` (structure-only clean), `seed records : N (regate_fail=0)`, `wrote learn.seed`. Exit code 0. If leaks>0 (exit 2) or regate_fail>0 (exit 3), STOP and report — the firmware learn path itself is leaking, which is a firmware finding, not an audit-tool gap.

- [ ] **Step 4: Record the seed size**

Note N (seed record count) for Task 3 interpretation. No commit.

---

### Task 2: `synth_dump --learn` — load a seed into the real learn store

**Files:**
- Modify: `tools/decoy_audit/synth_dump.c`
- Create: `tools/decoy_audit/host_stubs/nvs.h` (copy/adapt from `tools/pcap_learn/host_stubs/nvs.h`)
- Delete from the build: `tools/decoy_audit/learn_stub.c` is replaced by the real `learn.c` (+ `law3.c`, `learn_wire.c`); keep a 1-line `rf_model_load_nvs` stub (roster references it) in a new tiny `tools/decoy_audit/roster_stub.c` or fold into `synth_dump.c`.
- Modify: `tools/decoy_audit/run.ps1`, `tools/decoy_audit/Makefile`, `tools/decoy_audit/README.md` (build source list).

**Interfaces:**
- Consumes: `learn.h` (`learn_store_add`, `learn_count`, `learned_template_t`), the LSD1 seed format (Global Constraints).
- Produces: `synth_dump <seed> <n> <model.seed> [learn.seed]` — when the 4th arg is present, load it into the store before `generate_roster`, so `build_for_vendor` reproduces learned shapes.

- [ ] **Step 1: Write the failing test (loader round-trip)**

In `tools/decoy_audit/tests/test_synth_dump.py`, add a test that, given a tiny hand-built LSD1 seed with one record whose `family`/`company` are known, `synth_dump --learn` emits at least one row whose `arch` is in the learned range (`>= templates_count()`). (Exact assertion refined in Step 4; this documents intent.)

- [ ] **Step 2: Add the seed loader to `synth_dump.c`**

```c
/* Load a pcap_learn LSD1 seed (8-byte header + 55-byte LE records) into the real learn
   store, so generate_roster reproduces learned shapes. Returns records loaded, -1 on error. */
static int load_learn_seed(const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) return -1;
    unsigned char h[8];
    if (fread(h,1,8,fp)!=8) { fclose(fp); return -1; }
    uint32_t magic = h[0]|(h[1]<<8)|(h[2]<<16)|((uint32_t)h[3]<<24);
    uint16_t cnt = h[6]|(h[7]<<8);
    if (magic != 0x4C534431u) { fclose(fp); return -1; }
    int n = 0;
    for (uint16_t i=0;i<cnt;i++) {
        unsigned char b[55]; if (fread(b,1,55,fp)!=55) break;
        learned_template_t t; memset(&t,0,sizeof t); int p=0;
        memcpy(t.ad,b+p,31); p+=31;
        t.ad_len=b[p++]; t.name_off=b[p++]; t.name_len=b[p++];
        t.rand_mask=b[p]|(b[p+1]<<8)|(b[p+2]<<16)|((uint32_t)b[p+3]<<24); p+=4;
        t.company_id=b[p]|(b[p+1]<<8); p+=2;
        t.svc_uuid=b[p]|(b[p+1]<<8); p+=2;
        t.family=b[p++];
        t.itvl_min_ms=b[p]|(b[p+1]<<8); p+=2;
        t.itvl_max_ms=b[p]|(b[p+1]<<8); p+=2;
        t.shape_hash=b[p]|(b[p+1]<<8)|(b[p+2]<<16)|((uint32_t)b[p+3]<<24); p+=4;
        t.reinforce_count=b[p]|(b[p+1]<<8); p+=2;
        t.last_seen_sweep=b[p]|(b[p+1]<<8); p+=2;
        if (learn_store_add(&t, t.last_seen_sweep)) n++;
    }
    fclose(fp); return n;
}
```
Include `"learn.h"` in `synth_dump.c`. In the default `main` path, after `srand(seed)` and before `generate_roster`, add: if `argc > 4`, `load_learn_seed(argv[4])` (log the count to stderr).

- [ ] **Step 3: Rewire the build (real learn.c instead of the stub)**

`nvs.h` stub: copy `tools/pcap_learn/host_stubs/nvs.h` to `tools/decoy_audit/host_stubs/nvs.h` (provides `nvs_handle_t`, `nvs_open/close/commit/get_blob/set_blob`, `NVS_READONLY/READWRITE` as no-ops so `learn.c`'s save/load compile; they are never called).
`rf_model_load_nvs`: keep the 1-liner from the old `learn_stub.c` in a new `roster_stub.c` (the real `learn.c` does not provide it).
Update the `cl` line (run.ps1 build block + README) to replace `learn_stub.c` with:
`..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c roster_stub.c` and add `/I..\..\components\simulacra_radar` (already present).
Verify: `synth_dump.exe` builds; a plain `synth_dump 1 64 <model.seed>` (no 4th arg) still produces byte-identical output to before (learn_count()==0 when no seed loaded).

- [ ] **Step 4: Make the loader test pass**

Build a minimal LSD1 seed in the test (Python `struct`): header + one 55-byte record with `family=FMT_VENDOR_MFG`-ish and a company that the fallback model samples; run `synth_dump 1 128 <fallback-or-tmp-seed> <tmp.learn.seed>`; assert at least one row has `arch >= <templates_count>` (learned range). Determine `templates_count` from a no-learn run's max `arch`+1 or hardcode the known count. Run: green.

- [ ] **Step 5: Commit**

`git add tools/decoy_audit/{synth_dump.c,host_stubs/nvs.h,roster_stub.c,run.ps1,Makefile,README.md,tests/test_synth_dump.py}` — commit "feat(decoy_audit): synth_dump --learn loads a learn.seed into the real learn store".

---

### Task 3: Score the learned decoys + interpret

**Files:** none committed (outputs to `private/`); findings roll into the README in Task 4.

**Interfaces:**
- Consumes: `private/learn.seed` (Task 1), `synth_dump --learn` (Task 2), `private/{model.seed,profile.json}`.

- [ ] **Step 1: Generate learned decoys**

`.\synth_dump.exe 1 256 ..\..\private\model.seed ..\..\private\learn.seed | Out-File -Encoding ascii ..\..\private\synth_learned.ndjson`
Expected: stderr logs the loaded seed count; NDJSON has 256 rows, some with `arch` in the learned range.

- [ ] **Step 2: Score vs the real profile**

`python scorecard.py ..\..\private\synth_learned.ndjson ..\..\private\profile.json`
Expected: a headline. Record all four tells for the learned-enabled crowd.

- [ ] **Step 3: Compare learned-on vs learned-off**

Diff the headline/tells against the Task-0 baseline (learned-off, current main). Interpret honestly: does enabling learned shapes lower, raise, or leave the tells unchanged? A rise on any tell = `learn_render` introduces a detectable artifact the firmware ships (a real finding). Note the % of rows that were learned-derived (arch >= templates_count).

---

### Task 4: Tests, docs, regression

**Files:**
- Modify: `tools/decoy_audit/README.md`, `tools/decoy_audit/tests/test_synth_dump.py`

- [ ] **Step 1: Loader unit test**

Ensure the Task-2 loader test asserts both a well-formed seed loads (>=1 learned-range row) AND a bad-magic file is ignored (no crash, learn_count stays 0 -> all rows in template range). Run: green.

- [ ] **Step 2: README — learned-shape section**

Add a "Learned-shape audit" section: how to produce `learn.seed` (pcap_learn harness), how to run `synth_dump --learn`, the measured learned-on vs learned-off result from Task 3, and the honest interpretation. Note it validates the strip→render round-trip (the flagship self-learning path).

- [ ] **Step 3: Full host test run**

`python -m unittest discover -s tests` + `python -m unittest tests.test_ble_devices` — all green.

- [ ] **Step 4: Commit**

Commit "docs(decoy_audit): learned-shape audit results + loader tests".

---

## Notes / open questions for execution

- **MSVC packed-struct risk:** `learned_template_t` is `__attribute__((packed))`. Confirm `learn_record.h` neutralizes `__attribute__` under `_MSC_VER` (like `cyd/main/fleet_status.h` does); if not, the explicit 55-byte (de)serialization sidesteps host padding anyway — the *in-memory* struct just must not be memcpy'd to/from the file (the loader already does field-by-field, good). But `learn.c`'s internal store uses the struct directly; padding only matters for the wire/file, which we handle explicitly.
- **Interval override:** `generate_roster` overrides the learned template's interval with the model's `other_itvl_bins` for no-mfg mass; learned mfg vendors keep the vendor histogram interval. Confirm the learned interval band survives where it should (build_for_vendor learned branch sets `itvl` from `learn_render`).
- **Design choice (resolved):** measure MIXED (learning enabled, as firmware runs), not learned-only. The seed populates the store; `generate_roster` uses learned shapes where a learned template matches the sampled company, exactly as on-device.
