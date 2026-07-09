# Decoy Detectability Audit — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a host-side tool that scores how separable Simulacra's synthetic decoys are from a real ambient BLE crowd, producing a ranked detectability scorecard and a single regression-gate number.

**Architecture:** Compile the *real* firmware generation path (`generate.c`/`templates.c`/`roster.c`) against host stubs and dump a labeled synthetic decoy population as NDJSON (`synth_dump`). Parse a real BLE capture into an aggregate distribution profile (`capture_profile.py`). Score each discriminator as the Jensen–Shannon divergence between the synthetic and real distributions; the headline is the worst (max) tell (`discriminators.py` + `scorecard.py`).

**Tech Stack:** C (compiled with MSVC `cl` on this host; portable Makefile for gcc/clang CI), Python 3.12 stdlib only (`unittest`, `json`, `struct` — no third-party deps).

## Global Constraints

- **MSVC-only host:** the build machine has no gcc/clang. C builds use `cl` with a force-included shim (`/FI portab.h` where `portab.h` is `#define __attribute__(x)`), mirroring `tools/pcap_learn`. Provide a `Makefile` (uses `cc`) for gcc/clang CI as well.
- **Python:** stdlib only. Tests use `python -m unittest`. Do **not** add a pytest/numpy dependency.
- **Privacy (public repo):** no absolute local paths or OS usernames in any committed file. `capture_profile.py` output retains only aggregate distributions — never device addresses, names, or raw AD payloads. Captures and their parsed intermediates live under the gitignored `private/`.
- **Determinism:** `synth_dump` seeds the RNG from a CLI argument (`srand(seed)`) so a fixed seed yields a reproducible population and a reproducible headline score.
- **Include-order rule:** host stubs must win over `main/` headers of the same name. Compile with `-Ihost_stubs` (or `/Ihost_stubs`) **before** `-I../../main`. This shadows `churn.h`, `learn.h`, `esp_random.h`, `esp_log.h`, `sdkconfig.h`; the real `main/` `generate.h`/`identity.h`/`rf_model.h`/`roster.h`/`templates.h`/`trace.h` are used unshadowed.
- **Commit trailers:** keep `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` on commits (project directive). Work on branch `feat/decoy-audit` (already created); do not commit to `main`.
- **Repo-relative paths only** in this doc and in code: tool lives at `tools/decoy_audit/`.

---

## File Structure

```
tools/decoy_audit/
  host_stubs/
    portab.h            # #define __attribute__(x)   (MSVC packed-attr neutralizer)
    esp_log.h           # no-op ESP_LOG* macros (copy of pcap_learn stub)
    esp_random.h        # rand()-based esp_random / esp_fill_random (copy of pcap_learn stub)
    sdkconfig.h         # empty (copy of pcap_learn stub)
    churn.h             # #define CHURN_ACTIVE_SET 8
    learn.h             # minimal learned_template_t + LEARN_CAP + learn_* protos (learned path disabled)
    ble_uuid.h          # ble_uuid16_t + BLE_UUID16_INIT
    ble_hs.h            # struct ble_hs_adv_fields + flag consts + BLE_HS_ADV_MAX_SZ + set_fields proto
  ble_hs_adv.c          # host ble_hs_adv_set_fields (canonical-order TLV, length-correct)
  learn_stub.c          # learn_count()->0, learn_at()->NULL, learn_render()->1
  synth_dump.c          # main: read model-seed -> rf_model_t -> generate_roster -> NDJSON
  Makefile              # gcc/clang build (CI); MSVC invocation documented in README
  capture_profile.py    # pcap (Nordic DLT157 + DLT256, scan-for-AA) -> profile.json + model-seed
  discriminators.py     # address_type / interval / vendor separability (Jensen-Shannon)
  scorecard.py          # CLI: synth NDJSON + profile.json -> ranked scorecard + headline
  README.md
  tests/
    test_capture_profile.py
    test_discriminators.py
    test_scorecard.py
    make_fixtures.py     # writes tiny Nordic + DLT256 pcap fixtures used by tests
```

---

### Task 1: `synth_dump` — run the real generator on the host

**Files:**
- Create: `tools/decoy_audit/host_stubs/portab.h`, `esp_log.h`, `esp_random.h`, `sdkconfig.h`, `churn.h`, `learn.h`, `ble_uuid.h`, `ble_hs.h`
- Create: `tools/decoy_audit/ble_hs_adv.c`, `tools/decoy_audit/learn_stub.c`, `tools/decoy_audit/synth_dump.c`, `tools/decoy_audit/Makefile`
- Test: `tools/decoy_audit/tests/test_synth_dump.py`

**Interfaces:**
- Consumes: real `main/generate.c` `size_t generate_roster(const rf_model_t *m, identity_t *roster, size_t n)`; real `main/roster.c` `void make_random_static_addr_pub(uint8_t out[6])`; real `main/templates.c`.
- Produces: an executable `synth_dump` that prints one NDJSON object per identity to stdout:
  `{"addr":"<12 hex>","atype":"static|rpa|public","company":<int>,"itvl_ms":<int>,"tx":<int>,"arch":<int>,"plen":<int>}`
  Invocation for this task: `synth_dump <seed> <n>` with a **hardcoded** rf_model (real model-seed input arrives in Task 2).

- [ ] **Step 1: Copy the three reusable stubs**

Copy verbatim from `tools/pcap_learn/host_stubs/`: `esp_log.h`, `esp_random.h`, `sdkconfig.h` into `tools/decoy_audit/host_stubs/`. (Do **not** copy `templates.h` — this tool uses the real `main/templates.h`.)

- [ ] **Step 2: Write `host_stubs/portab.h`**

```c
#define __attribute__(x)
```

- [ ] **Step 3: Write `host_stubs/churn.h`**

```c
#pragma once
/* host stub: generate.c uses only CHURN_ACTIVE_SET (from generate_active_target). */
#define CHURN_ACTIVE_SET 8
```

- [ ] **Step 4: Write `host_stubs/learn.h`** (disables the learned-template path; `learn_count()` returns 0 so the branch in `generate.c` is never taken)

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#define LEARN_CAP 32
typedef struct { uint16_t company_id; } learned_template_t;  /* minimal: learned path is disabled */
size_t                    learn_count(void);
const learned_template_t *learn_at(size_t i);
int                       learn_render(const learned_template_t *lt, uint8_t out[31],
                                       uint8_t *len, uint16_t *itvl);
```

- [ ] **Step 5: Write `learn_stub.c`**

```c
#include "learn.h"
size_t                    learn_count(void) { return 0; }
const learned_template_t *learn_at(size_t i) { (void)i; return 0; }
int                       learn_render(const learned_template_t *lt, uint8_t out[31],
                                       uint8_t *len, uint16_t *itvl)
{ (void)lt; (void)out; (void)len; (void)itvl; return 1; }
```

- [ ] **Step 6: Write `host_stubs/ble_uuid.h`**

```c
#pragma once
#include <stdint.h>
typedef struct { uint8_t type; uint16_t value; } ble_uuid16_t;
#define BLE_UUID16_INIT(v) { 0x01, (v) }
```

- [ ] **Step 7: Write `host_stubs/ble_hs.h`** (only the fields `templates.c` sets)

```c
#pragma once
#include <stdint.h>
#include "ble_uuid.h"
#define BLE_HS_ADV_MAX_SZ        31
#define BLE_HS_ADV_F_DISC_GEN    0x02
#define BLE_HS_ADV_F_BREDR_UNSUP 0x04
struct ble_hs_adv_fields {
    const uint8_t     *mfg_data;        uint8_t mfg_data_len;
    uint8_t            flags;
    const uint8_t     *name;            uint8_t name_len; unsigned name_is_complete:1;
    const ble_uuid16_t *uuids16;        uint8_t num_uuids16; unsigned uuids16_is_complete:1;
    const uint8_t     *svc_data_uuid16; uint8_t svc_data_uuid16_len;
};
/* Serialize present fields into AD TLVs in NimBLE's canonical relative order:
   flags(0x01), uuids16(0x02/0x03), name(0x08/0x09), svc_data16(0x16), mfg(0xFF).
   Returns 0 on success, 1 if the result would exceed max_sz. */
int ble_hs_adv_set_fields(const struct ble_hs_adv_fields *f, uint8_t *buf,
                          uint8_t *out_len, uint8_t max_sz);
```

- [ ] **Step 8: Write `ble_hs_adv.c`**

```c
#include <string.h>
#include "ble_hs.h"
static int put(uint8_t *buf, uint8_t *n, uint8_t max, uint8_t type,
               const uint8_t *val, uint8_t vlen)
{
    if ((int)*n + 2 + vlen > max) return 1;
    buf[(*n)++] = (uint8_t)(1 + vlen);
    buf[(*n)++] = type;
    if (vlen) { memcpy(buf + *n, val, vlen); *n += vlen; }
    return 0;
}
int ble_hs_adv_set_fields(const struct ble_hs_adv_fields *f, uint8_t *buf,
                          uint8_t *out_len, uint8_t max_sz)
{
    uint8_t n = 0;
    if (f->flags && put(buf, &n, max_sz, 0x01, &f->flags, 1)) return 1;
    if (f->num_uuids16) {
        uint8_t u[2] = { (uint8_t)(f->uuids16->value & 0xff),
                         (uint8_t)(f->uuids16->value >> 8) };
        if (put(buf, &n, max_sz, f->uuids16_is_complete ? 0x03 : 0x02, u, 2)) return 1;
    }
    if (f->name && f->name_len &&
        put(buf, &n, max_sz, f->name_is_complete ? 0x09 : 0x08, f->name, f->name_len)) return 1;
    if (f->svc_data_uuid16 && f->svc_data_uuid16_len &&
        put(buf, &n, max_sz, 0x16, f->svc_data_uuid16, f->svc_data_uuid16_len)) return 1;
    if (f->mfg_data && f->mfg_data_len &&
        put(buf, &n, max_sz, 0xFF, f->mfg_data, f->mfg_data_len)) return 1;
    *out_len = n;
    return 0;
}
```

- [ ] **Step 9: Write `synth_dump.c`** (hardcoded rf_model for now; Task 2 replaces the model init)

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rf_model.h"
#include "identity.h"
#include "generate.h"

static const char *atype_of(const uint8_t addr[6]) {
    switch (addr[5] >> 6) { case 3: return "static"; case 1: return "rpa";
                            case 0: return "public"; default: return "nrpa"; }
}
int main(int argc, char **argv) {
    unsigned seed = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 10) : 1;
    size_t   n    = (argc > 2) ? (size_t)strtoul(argv[2], 0, 10) : 64;
    srand(seed);
    rf_model_t m; memset(&m, 0, sizeof(m));
    m.magic = RF_MODEL_MAGIC; m.version = RF_MODEL_VERSION;
    /* hardcoded two-vendor model so the generator has something to sample (Task 2: real seed) */
    m.vendors[0].company_id = 0x0075; m.vendors[0].count = 30;
    m.vendors[0].itvl_bins[2] = 30;   /* 100-200ms bin */
    m.vendors[1].company_id = 0x004C; m.vendors[1].count = 10;
    m.vendors[1].itvl_bins[2] = 10;
    m.other_count = 10; m.other_itvl_bins[3] = 10;
    m.total_obs = 50; m.pop_ewma = 12.0f;
    if (n > 256) n = 256;
    static identity_t roster[256];
    generate_roster(&m, roster, n);
    for (size_t i = 0; i < n; i++) {
        identity_t *id = &roster[i];
        char hex[13];
        for (int b = 0; b < 6; b++) sprintf(hex + b*2, "%02x", id->addr[b]);
        printf("{\"addr\":\"%s\",\"atype\":\"%s\",\"company\":%u,\"itvl_ms\":%u,"
               "\"tx\":%d,\"arch\":%u,\"plen\":%u}\n",
               hex, atype_of(id->addr), id->company_id, id->adv_itvl_ms,
               id->tx_power, id->archetype_idx, id->payload_len);
    }
    return 0;
}
```

- [ ] **Step 10: Write `Makefile`**

```make
CC ?= cc
ROOT := ../..
SRC := synth_dump.c ble_hs_adv.c learn_stub.c \
       $(ROOT)/main/generate.c $(ROOT)/main/templates.c $(ROOT)/main/roster.c
INC := -Ihost_stubs -I$(ROOT)/main
CFLAGS ?= -O2 -Wall -Wno-unused-parameter
all: synth_dump
synth_dump: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o synth_dump
clean:
	rm -f synth_dump synth_dump.exe
```

- [ ] **Step 11: Write the failing test `tests/test_synth_dump.py`**

```python
import json, os, subprocess, sys, unittest
HERE = os.path.dirname(__file__)
TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")

@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class SynthDump(unittest.TestCase):
    def rows(self, seed=1, n=64):
        out = subprocess.check_output([EXE, str(seed), str(n)], text=True)
        return [json.loads(l) for l in out.splitlines() if l.strip()]
    def test_emits_n_rows_with_keys(self):
        r = self.rows(n=64)
        self.assertEqual(len(r), 64)
        for k in ("addr","atype","company","itvl_ms","tx","arch","plen"):
            self.assertIn(k, r[0])
    def test_all_addresses_static_random(self):
        # decoys always use make_random_static_addr_pub -> static
        self.assertTrue(all(x["atype"] == "static" for x in self.rows()))
    def test_intervals_and_company_populated(self):
        r = self.rows()
        self.assertTrue(all(x["itvl_ms"] > 0 for x in r))
        self.assertTrue(any(x["company"] in (0x0075, 0x004C) for x in r))
    def test_deterministic_for_seed(self):
        self.assertEqual(self.rows(seed=7), self.rows(seed=7))

if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 12: Build (MSVC on this host) and run the test to see it pass**

Build:
```
cl /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIhost_stubs\portab.h ^
   /Ihost_stubs /I..\..\main ^
   synth_dump.c ble_hs_adv.c learn_stub.c ^
   ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ^
   /Fe:synth_dump.exe
```
Run: `python -m unittest tests.test_synth_dump -v`
Expected: 4 tests pass. (On a gcc/clang CI box, `make` instead of `cl`.)

- [ ] **Step 13: Commit**

```
git add tools/decoy_audit/host_stubs tools/decoy_audit/ble_hs_adv.c tools/decoy_audit/learn_stub.c tools/decoy_audit/synth_dump.c tools/decoy_audit/Makefile tools/decoy_audit/tests/test_synth_dump.py
git commit -m "feat(decoy_audit): run real firmware generator on host, dump labeled decoy population

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: real model-seed input for `synth_dump`

**Files:**
- Modify: `tools/decoy_audit/synth_dump.c` (replace hardcoded model with a seed-file reader)
- Test: `tools/decoy_audit/tests/test_synth_dump.py` (extend)

**Interfaces:**
- Consumes: a model-seed text file (produced by `capture_profile.py` in Task 3), format:
  ```
  POP <float>
  V <company_hex> <count> <b0> <b1> <b2> <b3> <b4> <b5> <b6>
  OTHER <count> <b0> <b1> <b2> <b3> <b4> <b5> <b6>
  ```
  `b0..b6` are the 7 `RF_ITVL_BINS` counts. Lines starting `#` or blank are ignored. Up to 24 `V` lines.
- Produces: `synth_dump <seed> <n> <model_seed_path>` builds the `rf_model_t` from that file. With no path arg, falls back to the Task-1 hardcoded model (keeps existing tests green).

- [ ] **Step 1: Write the failing test (extend `test_synth_dump.py`)**

```python
    def test_model_seed_biases_vendor_mix(self):
        seed_path = os.path.join(HERE, "_tmp_seed.txt")
        with open(seed_path, "w") as fh:
            fh.write("POP 10\n")
            fh.write("V 0087 100 0 0 0 0 0 40 0\n")   # Garmin-heavy, 1000-2000ms bin
            fh.write("OTHER 0 0 0 0 0 0 0 0\n")
        out = subprocess.check_output([EXE, "3", "128", seed_path], text=True)
        rows = [json.loads(l) for l in out.splitlines() if l.strip()]
        os.remove(seed_path)
        share = sum(1 for r in rows if r["company"] == 0x0087) / len(rows)
        self.assertGreater(share, 0.4)   # a Garmin-dominated model yields a Garmin-heavy crowd
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python -m unittest tests.test_synth_dump.SynthDump.test_model_seed_biases_vendor_mix -v`
Expected: FAIL (third arg currently ignored; mix not biased).

- [ ] **Step 3: Add the seed reader to `synth_dump.c`**

Add this function above `main` and call it when `argc > 3`:
```c
static int load_model_seed(const char *path, rf_model_t *m) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 1;
    char line[256]; size_t v = 0; uint64_t total = 0;
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!strncmp(line, "POP", 3)) { sscanf(line + 3, "%f", &m->pop_ewma); continue; }
        if (!strncmp(line, "OTHER", 5)) {
            uint32_t c, b[7] = {0};
            sscanf(line + 5, "%u %u %u %u %u %u %u %u", &c, &b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6]);
            m->other_count = c; total += c;
            for (int i=0;i<7;i++) m->other_itvl_bins[i] = b[i];
            continue;
        }
        if (line[0] == 'V' && v < RF_VENDOR_SLOTS) {
            unsigned cid, c, b[7] = {0};
            sscanf(line + 1, "%x %u %u %u %u %u %u %u %u", &cid, &c,
                   &b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6]);
            m->vendors[v].company_id = (uint16_t)cid; m->vendors[v].count = c; total += c;
            for (int i=0;i<7;i++) m->vendors[v].itvl_bins[i] = b[i];
            v++;
        }
    }
    fclose(fp);
    m->total_obs = (uint32_t)total;
    return 0;
}
```
In `main`, after zeroing `m` and setting magic/version, replace the hardcoded vendor block with:
```c
    if (argc > 3) { if (load_model_seed(argv[3], &m)) { fprintf(stderr, "seed load failed\n"); return 2; } }
    else { /* Task-1 fallback model */
        m.vendors[0].company_id = 0x0075; m.vendors[0].count = 30; m.vendors[0].itvl_bins[2] = 30;
        m.vendors[1].company_id = 0x004C; m.vendors[1].count = 10; m.vendors[1].itvl_bins[2] = 10;
        m.other_count = 10; m.other_itvl_bins[3] = 10; m.total_obs = 50; m.pop_ewma = 12.0f;
    }
```

- [ ] **Step 4: Rebuild and run the full test file**

Rebuild with the Step-12 `cl` command from Task 1. Run: `python -m unittest tests.test_synth_dump -v`
Expected: all tests pass (5 now).

- [ ] **Step 5: Commit**

```
git add tools/decoy_audit/synth_dump.c tools/decoy_audit/tests/test_synth_dump.py
git commit -m "feat(decoy_audit): drive synth population from a real model-seed file

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `capture_profile.py` — real crowd → distribution profile + model-seed

**Files:**
- Create: `tools/decoy_audit/capture_profile.py`, `tools/decoy_audit/tests/make_fixtures.py`, `tools/decoy_audit/tests/test_capture_profile.py`

**Interfaces:**
- Consumes: a pcap path (DLT256 or Nordic DLT157). Locates the PDU by scanning each record for the advertising Access Address `d6 be 89 8e`. Address type from the AdvA MSB top-2-bits (`3=static,1=rpa,0/other=public`) — **not** the PDU TxAdd bit (unreliable in the Nordic framing; see `private/Skills/nordic-pcap-parsing.md`).
- Produces two files (paths given as CLI args):
  - `profile.json`: `{"n_adverts":int,"n_addrs":int,"atype":{"static":f,"rpa":f,"public":f},"itvl_bins":[7 floats],"vendor":{"<cid>":f,...}}` — all distributions normalized to sum 1.0; **no addresses/names/AD retained**.
  - model-seed text (the Task-2 format), so a synth population can be generated matched to this crowd.
- Public functions (imported by tests): `parse_adverts(path) -> list[dict]` (each `{ts,addr,atype,company,itvl_bins_idx?}`), `build_profile(adverts) -> dict`, `write_model_seed(profile, path)`.

- [ ] **Step 1: Write `tests/make_fixtures.py`** (emits a tiny Nordic-DLT157 pcap the tests read)

```python
import struct, os
AA = bytes.fromhex("d6be898e")
def nordic_record(adva6, ad, rssi=-60, chan=38):
    # 17-byte Nordic header: [0]=04 [1]=06 [2]=paylen [3..7]... [off-8]=chan [off-7]=rssi(mag)
    hdr = bytearray(17)
    hdr[0]=0x04; hdr[1]=0x06; hdr[9]=chan & 0xff; hdr[10]=(-rssi) & 0xff
    pdu = bytes([0x02, 6+len(ad)]) + adva6 + ad        # ADV_NONCONN_IND
    payload = bytes(hdr) + AA + pdu
    hdr[2] = len(payload) - 7
    return bytes(hdr) + AA + pdu
def write_pcap(path, records):
    with open(path,"wb") as f:
        f.write(struct.pack("<IHHIIII", 0xa1b2c3d4,2,4,0,0,65535,157))
        for r in records:
            f.write(struct.pack("<IIII",0,0,len(r),len(r))); f.write(r)
def mfg(cid): return bytes([0x02,0x01,0x06, 0x04,0xFF, cid & 0xff, cid>>8, 0x11])
ADVAS=[(bytes([1,2,3,4,5,0xC0]),0x0075),(bytes([1,2,3,4,5,0xC1]),0x0075),
       (bytes([9,9,9,9,9,0xD0]),0x0087),(bytes([1,2,3,4,5,0x40]),0x004C),  # RPA
       (bytes([1,2,3,4,5,0x00]),0x0059)]                                    # public
def sample_pcap(path):
    # Nordic DLT157: 3 static-random, 1 RPA, 1 public
    write_pcap(path, [nordic_record(a, mfg(c)) for a,c in ADVAS])
def dlt256_record(adva6, ad):
    phdr=bytes(10)                                     # 10-byte LE_LL_WITH_PHDR pseudo-header
    pdu=bytes([0x02, 6+len(ad)]) + adva6 + ad
    return phdr + AA + pdu
def sample_pcap_dlt256(path):
    with open(path,"wb") as f:
        f.write(struct.pack("<IHHIIII", 0xa1b2c3d4,2,4,0,0,65535,256))
        for a,c in ADVAS:
            r=dlt256_record(a, mfg(c)); f.write(struct.pack("<IIII",0,0,len(r),len(r))); f.write(r)
if __name__ == "__main__":
    sample_pcap(os.path.join(os.path.dirname(__file__),"sample_nordic.pcap"))
    sample_pcap_dlt256(os.path.join(os.path.dirname(__file__),"sample_dlt256.pcap"))
```

- [ ] **Step 2: Write the failing test `tests/test_capture_profile.py`**

```python
import os, sys, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL); sys.path.insert(0, HERE)
import make_fixtures, capture_profile as cp

class Profile(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pcap = os.path.join(HERE, "sample_nordic.pcap")
        make_fixtures.sample_pcap(cls.pcap)
        cls.adv = cp.parse_adverts(cls.pcap)
        cls.prof = cp.build_profile(cls.adv)
    def test_parses_all_five(self):
        self.assertEqual(len(self.adv), 5)
    def test_address_types(self):
        at = self.prof["atype"]
        self.assertAlmostEqual(at["static"], 3/5)
        self.assertAlmostEqual(at["rpa"], 1/5)
        self.assertAlmostEqual(at["public"], 1/5)
    def test_vendor_distribution_sums_to_one(self):
        self.assertAlmostEqual(sum(self.prof["vendor"].values()), 1.0, places=6)
        self.assertIn(str(0x0075), self.prof["vendor"])
    def test_model_seed_roundtrip(self):
        seed = os.path.join(HERE, "_seed.txt")
        cp.write_model_seed(self.prof, seed)
        txt = open(seed).read(); os.remove(seed)
        self.assertIn("POP", txt); self.assertIn("V 0075", txt)
    def test_dlt256_decodes_identically(self):
        # the scan-for-AA reader must yield the same adverts for DLT256 framing
        p256 = os.path.join(HERE, "sample_dlt256.pcap")
        make_fixtures.sample_pcap_dlt256(p256)
        adv256 = cp.parse_adverts(p256)
        key = lambda a: (a["atype"], a["company"])
        self.assertEqual(sorted(map(key, adv256)), sorted(map(key, self.adv)))

if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 3: Run it to verify it fails**

Run: `python -m unittest tests.test_capture_profile -v`
Expected: FAIL — `ModuleNotFoundError: capture_profile`.

- [ ] **Step 4: Write `capture_profile.py`**

```python
#!/usr/bin/env python3
"""Real BLE crowd -> aggregate distribution profile + a model-seed for synth_dump.
DLT256 and Nordic DLT157 aware (scans for the advertising Access Address).
Privacy: emits only distributions; never addresses, names, or AD payloads."""
import sys, struct, json, statistics
from collections import defaultdict, Counter

AA = bytes.fromhex("d6be898e")
ITVL_LO = [0,50,100,200,500,1000,2000]; ITVL_HI = [50,100,200,500,1000,2000,3000]

def itvl_bin(ms):
    for i in range(7):
        if ITVL_LO[i] <= ms < ITVL_HI[i]: return i
    return 6

def _company(ad):
    i=0
    while i+1 < len(ad):
        l=ad[i]
        if l==0 or i+1+l>len(ad): break
        if ad[i+1]==0xFF and l>=3: return ad[i+2] | (ad[i+3]<<8)
        i+=1+l
    return 0

def _atype(msb):
    return {3:"static",1:"rpa"}.get(msb>>6,"public")

def parse_adverts(path):
    f=open(path,"rb"); f.read(24); out=[]
    while True:
        rh=f.read(16)
        if len(rh)<16: break
        ts_s,ts_u,incl,_=struct.unpack("<IIII",rh); data=f.read(incl)
        if len(data)<incl: break
        off=data.find(AA)
        if off<0 or off+6>len(data): continue
        pdu=data[off+4:]
        if len(pdu)<8: continue
        h0,plen=pdu[0],pdu[1]
        if (h0&0x0F) not in (0,2,6): continue
        body=pdu[2:2+plen]
        if len(body)<6: continue
        adva=body[:6]; ad=body[6:]
        out.append({"ts":ts_s+ts_u/1e6, "addr":adva[::-1].hex(),
                    "atype":_atype(adva[5]), "company":_company(ad)})
    return out

def build_profile(adverts):
    at=Counter(a["atype"] for a in adverts)
    ven=Counter(a["company"] for a in adverts if a["company"])
    # per-address median interval -> bin
    ts=defaultdict(list)
    for a in adverts: ts[a["addr"]].append(a["ts"])
    ibins=[0]*7
    for addr,t in ts.items():
        t.sort()
        gaps=[(t[i+1]-t[i])*1000 for i in range(len(t)-1) if 5<(t[i+1]-t[i])*1000<60000]
        if gaps: ibins[itvl_bin(statistics.median(gaps))]+=1
    n=len(adverts) or 1
    def norm(counter, keys=None):
        tot=sum(counter.values()) or 1
        return {str(k):counter[k]/tot for k in (keys or counter)}
    isum=sum(ibins) or 1
    return {"n_adverts":len(adverts),"n_addrs":len(ts),
            "atype":{k:at[k]/n for k in ("static","rpa","public")},
            "itvl_bins":[b/isum for b in ibins],
            "vendor":norm(ven)}

def write_model_seed(profile, path):
    # convert normalized vendor shares back into integer counts (scale 1000) + interval bins
    vend=profile["vendor"]; ib=profile["itvl_bins"]
    # spread the global interval histogram across vendors proportionally (coarse but sufficient)
    binc=[int(round(x*1000)) for x in ib]
    with open(path,"w") as f:
        f.write("POP 12\n")
        top=sorted(vend.items(), key=lambda kv:-kv[1])[:24]
        for cid,share in top:
            c=int(round(share*1000))
            f.write("V %04x %d %s\n" % (int(cid), c, " ".join(str(int(share*b)) for b in binc)))
        f.write("OTHER 0 " + " ".join(str(b) for b in binc) + "\n")

def main():
    adv=parse_adverts(sys.argv[1]); prof=build_profile(adv)
    json.dump(prof, open(sys.argv[2],"w"), indent=2)
    if len(sys.argv)>3: write_model_seed(prof, sys.argv[3])
    sys.stderr.write("adverts=%d addrs=%d\n"%(prof["n_adverts"],prof["n_addrs"]))

if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `python -m unittest tests.test_capture_profile -v`
Expected: 4 tests pass.

- [ ] **Step 6: Commit**

```
git add tools/decoy_audit/capture_profile.py tools/decoy_audit/tests/make_fixtures.py tools/decoy_audit/tests/test_capture_profile.py
git commit -m "feat(decoy_audit): real-crowd distribution profile + model-seed from a capture

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: `discriminators.py` — per-tell separability scores

**Files:**
- Create: `tools/decoy_audit/discriminators.py`, `tools/decoy_audit/tests/test_discriminators.py`

**Interfaces:**
- Consumes: `synth` (list of NDJSON dicts from `synth_dump`) and `profile` (dict from `build_profile`).
- Produces:
  - `js_divergence(p: list[float], q: list[float]) -> float` — Jensen–Shannon divergence, base-2, in `[0,1]`.
  - `synth_distributions(synth) -> dict` with keys `atype` (3-vector static/rpa/public), `itvl_bins` (7-vector), `vendor` (dict cid->share).
  - `DISCRIMINATORS`: list of `(name, fn)` where `fn(synth_dists, profile) -> float` separability in `[0,1]`.
  - `score_all(synth, profile) -> list[dict]`: `[{"name":str,"separability":float,"visibility":"logic"}...]`.

- [ ] **Step 1: Write the failing test `tests/test_discriminators.py`**

```python
import os, sys, unittest
HERE=os.path.dirname(__file__); TOOL=os.path.dirname(HERE); sys.path.insert(0,TOOL)
import discriminators as D

class JS(unittest.TestCase):
    def test_identical_is_zero(self):
        self.assertAlmostEqual(D.js_divergence([0.5,0.5],[0.5,0.5]), 0.0, places=6)
    def test_disjoint_is_one(self):
        self.assertAlmostEqual(D.js_divergence([1,0],[0,1]), 1.0, places=6)

class Scores(unittest.TestCase):
    def setUp(self):
        # synth: all static-random, all 100-200ms, all vendor 0x75
        self.synth=[{"atype":"static","itvl_ms":150,"company":0x75} for _ in range(50)]
        self.profile={"atype":{"static":0.4,"rpa":0.4,"public":0.2},
                      "itvl_bins":[0,0,0.2,0.2,0.2,0.2,0.2],
                      "vendor":{str(0x75):0.3, str(0x4c):0.7}}
    def test_address_type_tell_is_high(self):
        s={d["name"]:d["separability"] for d in D.score_all(self.synth, self.profile)}
        self.assertGreater(s["address_type_mix"], 0.3)  # 100% static vs 40/40/20 real
    def test_all_scores_in_unit_range(self):
        for d in D.score_all(self.synth, self.profile):
            self.assertGreaterEqual(d["separability"],0.0)
            self.assertLessEqual(d["separability"],1.0)
            self.assertEqual(d["visibility"],"logic")

if __name__=="__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python -m unittest tests.test_discriminators -v`
Expected: FAIL — `ModuleNotFoundError: discriminators`.

- [ ] **Step 3: Write `discriminators.py`**

```python
#!/usr/bin/env python3
"""Separability discriminators: how distinguishable is the synthetic decoy
population from the real crowd, per feature. Each returns a score in [0,1]
(0 = indistinguishable, 1 = trivially separable) via Jensen-Shannon divergence."""
import math
from collections import Counter

ITVL_LO=[0,50,100,200,500,1000,2000]; ITVL_HI=[50,100,200,500,1000,2000,3000]
def _itvl_bin(ms):
    for i in range(7):
        if ITVL_LO[i] <= ms < ITVL_HI[i]: return i
    return 6

def _norm(v):
    s=sum(v)
    return [x/s for x in v] if s else [0.0]*len(v)

def js_divergence(p, q):
    p=_norm(list(p)); q=_norm(list(q))
    m=[(pi+qi)/2 for pi,qi in zip(p,q)]
    def kl(a,b):
        s=0.0
        for ai,bi in zip(a,b):
            if ai>0 and bi>0: s+=ai*math.log2(ai/bi)
        return s
    jsd=0.5*kl(p,m)+0.5*kl(q,m)
    return max(0.0, min(1.0, jsd))

def synth_distributions(synth):
    at=Counter(x["atype"] for x in synth)
    n=len(synth) or 1
    ib=[0]*7
    for x in synth: ib[_itvl_bin(x["itvl_ms"])]+=1
    ven=Counter(x["company"] for x in synth if x.get("company"))
    vt=sum(ven.values()) or 1
    return {"atype":[at.get(k,0)/n for k in ("static","rpa","public")],
            "itvl_bins":_norm(ib),
            "vendor":{str(k):v/vt for k,v in ven.items()}}

def _vendor_vectors(sv, pv):
    keys=sorted(set(sv)|set(pv))
    return [sv.get(k,0) for k in keys],[pv.get(k,0) for k in keys]

def d_address_type(sd, prof):
    p=sd["atype"]; q=[prof["atype"].get(k,0) for k in ("static","rpa","public")]
    return js_divergence(p,q)
def d_interval(sd, prof):
    return js_divergence(sd["itvl_bins"], prof["itvl_bins"])
def d_vendor(sd, prof):
    s,p=_vendor_vectors(sd["vendor"], prof["vendor"]); return js_divergence(s,p)

DISCRIMINATORS=[("address_type_mix",d_address_type),
                ("interval_distribution",d_interval),
                ("vendor_histogram",d_vendor)]

def score_all(synth, profile):
    sd=synth_distributions(synth)
    return [{"name":n,"separability":round(fn(sd,profile),4),"visibility":"logic"}
            for n,fn in DISCRIMINATORS]
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python -m unittest tests.test_discriminators -v`
Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```
git add tools/decoy_audit/discriminators.py tools/decoy_audit/tests/test_discriminators.py
git commit -m "feat(decoy_audit): Jensen-Shannon separability discriminators (addr/interval/vendor)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: `scorecard.py` — ranked scorecard + headline + regression gate

**Files:**
- Create: `tools/decoy_audit/scorecard.py`, `tools/decoy_audit/tests/test_scorecard.py`

**Interfaces:**
- Consumes: `synth` NDJSON path, `profile.json` path.
- Produces:
  - `build_scorecard(synth, profile) -> dict`: `{"discriminators":[...sorted desc by separability...],"headline":float,"headline_tell":str}` where `headline = max` separability.
  - CLI `scorecard.py <synth.ndjson> <profile.json> [--json out.json] [--gate 0.5]`: prints a ranked text table + headline; exits `1` if `headline > gate` (regression gate), else `0`.

- [ ] **Step 1: Write the failing test `tests/test_scorecard.py`**

```python
import io, json, os, sys, unittest
HERE=os.path.dirname(__file__); TOOL=os.path.dirname(HERE); sys.path.insert(0,TOOL)
import scorecard as SC

class Card(unittest.TestCase):
    def setUp(self):
        self.synth=[{"atype":"static","itvl_ms":150,"company":0x75} for _ in range(30)]
        self.profile={"atype":{"static":0.4,"rpa":0.4,"public":0.2},
                      "itvl_bins":[0,0,1.0,0,0,0,0],
                      "vendor":{str(0x75):1.0}}
    def test_headline_is_max(self):
        card=SC.build_scorecard(self.synth,self.profile)
        self.assertEqual(card["headline"], max(d["separability"] for d in card["discriminators"]))
    def test_ranked_descending(self):
        ds=SC.build_scorecard(self.synth,self.profile)["discriminators"]
        self.assertEqual(ds, sorted(ds, key=lambda d:-d["separability"]))
    def test_headline_tell_matches(self):
        card=SC.build_scorecard(self.synth,self.profile)
        self.assertEqual(card["headline_tell"], card["discriminators"][0]["name"])
    def test_interval_and_vendor_indistinguishable_here(self):
        # synth matches real on interval(100-200) and vendor(0x75) -> those tells ~0
        card=SC.build_scorecard(self.synth,self.profile)
        s={d["name"]:d["separability"] for d in card["discriminators"]}
        self.assertLess(s["interval_distribution"],1e-6)
        self.assertLess(s["vendor_histogram"],1e-6)

if __name__=="__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python -m unittest tests.test_scorecard -v`
Expected: FAIL — `ModuleNotFoundError: scorecard`.

- [ ] **Step 3: Write `scorecard.py`**

```python
#!/usr/bin/env python3
"""Aggregate discriminators into a ranked detectability scorecard. The headline
score is the MAX separability (an adversary uses their single best tell), so the
worst tell defines exposure. Exit non-zero if headline exceeds a regression gate."""
import argparse, json, sys
import discriminators as D

def build_scorecard(synth, profile):
    ds=sorted(D.score_all(synth, profile), key=lambda d:-d["separability"])
    headline=ds[0]["separability"] if ds else 0.0
    return {"discriminators":ds,"headline":headline,
            "headline_tell":ds[0]["name"] if ds else None}

def _load_ndjson(path):
    return [json.loads(l) for l in open(path) if l.strip()]

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("synth"); ap.add_argument("profile")
    ap.add_argument("--json"); ap.add_argument("--gate", type=float, default=1.1)
    a=ap.parse_args()
    synth=_load_ndjson(a.synth); profile=json.load(open(a.profile))
    card=build_scorecard(synth, profile)
    print("%-24s %12s %s" % ("DISCRIMINATOR","SEPARABILITY","VISIBILITY"))
    for d in card["discriminators"]:
        print("%-24s %12.4f %s" % (d["name"], d["separability"], d["visibility"]))
    print("-"*50)
    print("HEADLINE (max) %.4f  worst tell: %s" % (card["headline"], card["headline_tell"]))
    if a.json: json.dump(card, open(a.json,"w"), indent=2)
    sys.exit(1 if card["headline"] > a.gate else 0)

if __name__=="__main__":
    main()
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python -m unittest tests.test_scorecard -v`
Expected: 4 tests pass. Then run the whole suite: `python -m unittest discover -s tests -v` — all green.

- [ ] **Step 5: Commit**

```
git add tools/decoy_audit/scorecard.py tools/decoy_audit/tests/test_scorecard.py
git commit -m "feat(decoy_audit): ranked scorecard + max-tell headline + regression gate

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: README + end-to-end run on the real capture

**Files:**
- Create: `tools/decoy_audit/README.md`

**Interfaces:**
- Consumes: everything above. Documents the full pipeline and records a real headline number.

- [ ] **Step 1: Run the end-to-end pipeline against the private capture**

```
# 1. real crowd -> profile + model-seed  (capture is gitignored under private/)
python capture_profile.py ../../private/long.pcap ../../private/profile.json ../../private/model.seed
# 2. synthetic decoys matched to that crowd (built synth_dump.exe from Task 1/2)
./synth_dump 1 256 ../../private/model.seed > ../../private/synth.ndjson
# 3. scorecard
python scorecard.py ../../private/synth.ndjson ../../private/profile.json --json ../../private/card.json
```
Record the printed headline and per-tell table (expect `address_type_mix` to dominate: synth is 100% static-random vs the real ~43/30/10 static/rpa/public mix).

- [ ] **Step 2: Write `README.md`**

Document: purpose (one paragraph), the three units, the MSVC `cl` build line (from Task 1 Step 12) **and** the `make` alternative, the Python commands above, the scoring method (JS divergence; headline = max tell), the regression-gate usage (`--gate`), the deferred slices (byte-accurate NimBLE serializer for an AD-structure discriminator; physical-only discriminators — RSSI/TX spread, lifespan cohort — needing a decoy-only capture; learned-shape path currently disabled via `learn_stub.c`). State the privacy rule: captures and `profile.json`/`*.ndjson`/`*.seed` stay under `private/`, and `profile.json` holds only distributions. **No absolute paths or usernames anywhere in the file.**

- [ ] **Step 3: Commit**

```
git add tools/decoy_audit/README.md
git commit -m "docs(decoy_audit): README, build/run pipeline, scoring method, deferred slices

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Notes for the implementer

- **Scoring vs. the spec's calibration:** the spec described calibration as pinning a per-device classifier's false-positive rate on real devices, then measuring true-positive rate. v1 has only **distributional** discriminators (address-type mix, interval, vendor histogram), for which Jensen–Shannon divergence between the synthetic and real distributions is the natural separability measure — it is 0 when synth matches real (never "fires" on a faithful crowd) and 1 when disjoint. FPR-pinned **per-device** classification arrives with the physical-only slice (per-advert "looks synthetic" tells), where that calibration framing applies.
- **Learned-shape path is intentionally disabled** for v1 (`learn_stub.c` → `learn_count()==0`), so the audit measures the built-in-template generator. Enabling it (compile the real `main/learn.c` + a seeded library) is a later slice once the baseline is trusted.
- **`synth_dump` payload bytes are approximate** (length-correct, canonical-order TLVs). The v1 discriminators (address-type, interval, vendor) never read them, so scores are faithful. A byte-accurate NimBLE serializer is required only when the AD-structure discriminator lands.
- If `cl` reports an unknown-macro or missing-symbol error, check the include order (`/Ihost_stubs` before `/I..\..\main`) and that `ble_hs_adv.c` + `learn_stub.c` are in the source list.
- The real capture (`private/long.pcap`) is Nordic DLT157; `capture_profile.py` auto-handles it by scanning for the Access Address.
```
