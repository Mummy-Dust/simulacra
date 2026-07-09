# pcap → Learn Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** An offline host tool that replays the real drive-time BLE capture through the actual firmware `strip → Law-3 → shape_hash → dedup` pipeline, prints a validation report (incl. a structure-only audit), and emits a plaintext `learn.seed`; plus a Vigil boot-time import that re-gates the seed into its sealed `learn.db`.

**Architecture:** `tools/pcap_learn/` — `parse_pcap.py` (stdlib) extracts advertising PDUs → NDJSON; `harness.c` compiles the real `law3.c`/`learn_wire.c`/`learn.c` against tiny host stubs and runs the pipeline. Firmware change is one boot-time import block in `cyd/main/cyd_main.c`.

**Tech Stack:** Python 3 (stdlib only), C (host gcc/clang), the existing firmware C pipeline unchanged.

## Global Constraints

- Reuse the firmware pipeline **unchanged** — do not fork `learn_strip`/`learn_shape_hash`/`learn_merge`/`law3_forbidden`. The host builds the real `.c` files.
- The pcap and any emitted `learn.seed` are **local, uncommitted**. Only tool source is committed. Add `tools/pcap_learn/*.seed`, `*.ndjson`, and the built binary to a local ignore.
- Structure-only is an invariant: the audit is a hard gate. Any un-masked, un-named identity byte = FAIL.
- `fmt_family_t` stub must use the REAL values: `FMT_VENDOR_MFG=0, FMT_IBEACON=1, FMT_EDDYSTONE_UID=2, FMT_EDDYSTONE_URL=3, FMT_SVC_TRACKER=4`.
- `learned_template_t` is 55-byte PACKED (see `components/simulacra_radar/learn_record.h`).
- Advertising AA = `0x8E89BED6`; DLT 256 record = 10B phdr + 4B AA + PDU(2B hdr + payload).
- AI trailers stay in commits; PR body ends with the Claude Code line; push via the gh credential-helper form.
- Capture path: `~\.claude\uploads\a931c64f-07ba-48b6-93fa-afb5ea4e543b\75af4aa2-new.pcap`.

## File Structure

```
tools/pcap_learn/
  parse_pcap.py      # T1
  host_stubs/        # T2: esp_random.h, esp_log.h, nvs.h, sdkconfig.h, templates.h
  harness.c          # T2 skeleton, T3 report+audit, T4 seed emit
  Makefile           # T2
  README.md          # T4
  .gitignore         # T2 (ignore *.ndjson, *.seed, harness binary)
cyd/main/cyd_main.c   # T5: boot-time seed import
```

---

### Task 1: pcap parser → NDJSON

**Files:** Create `tools/pcap_learn/parse_pcap.py`

**Interfaces:** Produces NDJSON lines `{"company": <int>, "ad": "<hex>"}` on stdout; per-PDU-type counts to stderr.

- [ ] **Step 1: Write the parser.**

```python
#!/usr/bin/env python3
"""Extract legacy BLE advertising AD payloads from a DLT-256
(LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR) pcap. Emits one NDJSON object per advert:
{"company": <int>, "ad": "<hex of AD bytes>"}. Stdlib only."""
import sys, struct, json

ADV_AA = 0x8E89BED6
# PDU types (low nibble of PDU header byte 0) that carry an AD payload after AdvA(6):
AD_PAYLOAD_TYPES = {0: "ADV_IND", 2: "ADV_NONCONN_IND", 6: "ADV_SCAN_IND", 4: "SCAN_RSP"}

def company_of(ad: bytes) -> int:
    i = 0
    while i + 1 < len(ad):
        l = ad[i]
        if l == 0 or i + 1 + l > len(ad):
            break
        if ad[i+1] == 0xFF and l >= 3:           # manufacturer specific: company = LE16 of value
            return ad[i+2] | (ad[i+3] << 8)
        i += 1 + l
    return 0

def main():
    path = sys.argv[1]
    f = open(path, "rb")
    gh = f.read(24)
    magic, vma, vmi, tz, sig, snap, dlt = struct.unpack("<IHHIIII", gh)
    if dlt != 256:
        sys.stderr.write(f"warning: DLT={dlt}, expected 256\n")
    counts, emitted, recs = {}, 0, 0
    while True:
        rh = f.read(16)
        if len(rh) < 16:
            break
        ts, tu, incl, orig = struct.unpack("<IIII", rh)
        data = f.read(incl)
        if len(data) < incl:
            break
        recs += 1
        if len(data) < 16:
            continue
        aa = struct.unpack("<I", data[10:14])[0]
        if aa != ADV_AA:
            continue
        h0, plen = data[14], data[15]
        ptype = h0 & 0x0F
        counts[ptype] = counts.get(ptype, 0) + 1
        if ptype not in AD_PAYLOAD_TYPES:
            continue
        pdu = data[16:16+plen]                    # AdvA(6) + AD
        if len(pdu) < 6:
            continue
        ad = pdu[6:]
        if not ad:
            continue
        sys.stdout.write(json.dumps({"company": company_of(ad), "ad": ad.hex()}) + "\n")
        emitted += 1
    named = {AD_PAYLOAD_TYPES.get(k, f"type{k}"): v for k, v in sorted(counts.items())}
    sys.stderr.write(f"records={recs} adv_pdus={sum(counts.values())} emitted_ad={emitted}\n")
    sys.stderr.write(f"pdu_types={named}\n")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run on the real capture.**

```
python tools/pcap_learn/parse_pcap.py "~/.claude/uploads/a931c64f-07ba-48b6-93fa-afb5ea4e543b/75af4aa2-new.pcap" > tools/pcap_learn/adverts.ndjson
```
Expected (stderr): `records=<big>`, `emitted_ad` in the thousands, `pdu_types` showing ADV_IND/SCAN_RSP/etc. Spot-check the first NDJSON line has a plausible `company` and hex `ad`.

- [ ] **Step 3: Commit** (`git add tools/pcap_learn/parse_pcap.py`).

---

### Task 2: host stubs + Makefile + compiling skeleton

**Files:** Create `tools/pcap_learn/host_stubs/{esp_random.h,esp_log.h,nvs.h,sdkconfig.h,templates.h}`, `tools/pcap_learn/harness.c` (skeleton), `tools/pcap_learn/Makefile`, `tools/pcap_learn/.gitignore`

**Interfaces:** `make` produces `./harness` linking the real `law3.c`, `learn_wire.c`, `learn.c`.

- [ ] **Step 1: Stubs.**

`host_stubs/sdkconfig.h`: empty file (one comment line).

`host_stubs/esp_random.h`:
```c
#pragma once
#include <stdint.h>
#include <stdlib.h>
static inline uint32_t esp_random(void){ return ((uint32_t)rand()<<16) ^ (uint32_t)rand(); }
static inline void esp_fill_random(void *buf, size_t n){ unsigned char *p=buf; for(size_t i=0;i<n;i++) p[i]=(unsigned char)rand(); }
```

`host_stubs/esp_log.h`:
```c
#pragma once
#define ESP_LOGE(t,...) ((void)0)
#define ESP_LOGW(t,...) ((void)0)
#define ESP_LOGI(t,...) ((void)0)
#define ESP_LOGD(t,...) ((void)0)
#define ESP_LOGV(t,...) ((void)0)
```

`host_stubs/nvs.h`:
```c
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char*ns,nvs_open_mode_t m,nvs_handle_t*h){(void)ns;(void)m;*h=0;return -1;}
static inline esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*v,size_t*l){(void)h;(void)k;(void)v;(void)l;return -1;}
static inline esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*v,size_t l){(void)h;(void)k;(void)v;(void)l;return 0;}
static inline esp_err_t nvs_commit(nvs_handle_t h){(void)h;return 0;}
static inline void nvs_close(nvs_handle_t h){(void)h;}
```

`host_stubs/templates.h` (exact enum, plus the archetype struct learn.h's include expects — copy verbatim from `main/templates.h` lines 1-13; the struct is only referenced by generation code the harness never calls, so the enum alone is what `learn.c` needs, but include the struct decl too if the compiler asks):
```c
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef enum {
    FMT_VENDOR_MFG,
    FMT_IBEACON,
    FMT_EDDYSTONE_UID,
    FMT_EDDYSTONE_URL,
    FMT_SVC_TRACKER,
} fmt_family_t;
```

- [ ] **Step 2: harness.c skeleton** (compiles + links the real pipeline; reads NDJSON, runs strip+hash+merge, prints only a count for now):

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "learn.h"          // learn_strip
#include "learn_record.h"   // learned_template_t, learn_shape_hash
#include "learn_wire.h"     // learn_merge, learn_regate

#define CAP 4096
static learned_template_t store[CAP];
static size_t count;

static int hexbyte(const char *s){ int hi=s[0],lo=s[1];
    hi = hi<='9'?hi-'0':(hi|32)-'a'+10; lo = lo<='9'?lo-'0':(lo|32)-'a'+10; return (hi<<4)|lo; }

int main(int argc, char **argv){
    FILE *f = argc>1 ? fopen(argv[1],"r") : stdin;
    if(!f){ fprintf(stderr,"cannot open input\n"); return 1; }
    char line[512]; unsigned in=0, ok=0, rej=0; uint16_t sweep=0;
    while(fgets(line,sizeof line,f)){
        char *cp = strstr(line,"\"company\":"); char *ap = strstr(line,"\"ad\":\"");
        if(!cp||!ap) continue;
        unsigned company = (unsigned)strtoul(cp+10,NULL,10);
        char *hex = ap+6; char *end = strchr(hex,'"'); if(!end) continue;
        size_t hlen=(size_t)(end-hex); if(hlen%2) continue;
        uint8_t ad[64]; size_t adlen=hlen/2; if(adlen>31) adlen=31;
        for(size_t i=0;i<adlen;i++) ad[i]=(uint8_t)hexbyte(hex+2*i);
        in++;
        learned_template_t rec; memset(&rec,0,sizeof rec);
        if(!learn_strip(ad,(uint8_t)adlen,(uint16_t)company,&rec)){ rej++; continue; }
        rec.shape_hash = learn_shape_hash(&rec);
        learn_merge(store,&count,CAP,&rec,sweep++);
        ok++;
    }
    printf("in=%u parsed=%u rejected=%u unique_shapes=%zu\n", in, ok, rej, count);
    return 0;
}
```

- [ ] **Step 3: Makefile.**

```make
CC ?= cc
ROOT := ../..
SRC := harness.c $(ROOT)/components/simulacra_radar/law3.c \
       $(ROOT)/components/simulacra_radar/learn_wire.c $(ROOT)/main/learn.c
INC := -Ihost_stubs -I$(ROOT)/components/simulacra_radar -I$(ROOT)/main
CFLAGS ?= -O2 -Wall -Wno-unused-parameter
harness: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o harness
clean:
	rm -f harness
```

- [ ] **Step 4: `.gitignore`** in `tools/pcap_learn/`:
```
*.ndjson
*.seed
harness
harness.exe
```

- [ ] **Step 5: Build.**

```
cd tools/pcap_learn && make
```
Expected: compiles clean and links. If `learn.c` references a `templates.h` symbol beyond `fmt_family_t`, the error names it — add a minimal declaration to the stub and rebuild. (Anticipated: enum only.)

- [ ] **Step 6: Smoke-run** on the real NDJSON:
```
./harness adverts.ndjson
```
Expected: `in=<thousands> parsed=<n> rejected=<m> unique_shapes=<k>` with k far smaller than parsed (dedup working).

- [ ] **Step 7: Commit** (stubs, harness.c, Makefile, .gitignore).

---

### Task 3: report + structure-only audit

**Files:** Modify `tools/pcap_learn/harness.c`

**Interfaces:** richer stdout report; nonzero exit if the structure-only audit fails.

- [ ] **Step 1: Add a Law-3 reject reason breakdown.** Since `learn_strip` returns only bool, classify rejects by re-checking cheaply in the harness: call the real `law3_forbidden(ad,len)` (declare via `#include "law3.h"`) to split "Law-3 forbidden" from "malformed/over-budget" (strip returned false but law3 allowed). Track `rej_law3`, `rej_other`. Print both.

- [ ] **Step 2: Add the structure-only audit.** For every record in `store`, re-walk `rec.ad` with the same AD-type rules `learn_strip` uses and assert each value byte is EITHER a preserved structural byte OR masked in `rand_mask` OR inside the name region. Add:

```c
// returns number of identity bytes NOT covered (0 = clean)
static int audit_record(const learned_template_t *r){
    int leak=0;
    for(uint8_t i=0;i+1<r->ad_len;){
        uint8_t l=r->ad[i]; if(l==0||i+1+l>r->ad_len) break;
        uint8_t type=r->ad[i+1], vfrom=i+2, vto=i+1+l;
        for(uint8_t b=vfrom;b<vto;b++){
            int preserved=0, masked=(r->rand_mask>>b)&1;
            int named = r->name_len && b>=r->name_off && b<r->name_off+r->name_len;
            switch(type){
                case 0x01: case 0x02: case 0x03: case 0x0A: case 0x19: preserved=1; break; // flags/uuid16/tx/appearance
                case 0x08: case 0x09: preserved=1; break; // name bytes handled via name region
                case 0xFF: if(b<vfrom+2) preserved=1;      // company id
                    else if(r->ad[vfrom]==0x4C&&r->ad[vfrom+1]==0x00&&(vto-vfrom)>=4&&r->ad[vfrom+2]==0x02&&r->ad[vfrom+3]==0x15&&b<vfrom+4) preserved=1; // iBeacon prefix
                    break;
                case 0x16: if(b<vfrom+2) preserved=1; break; // svc-data uuid
                default: break;                              // unknown: everything must be masked
            }
            if(!preserved && !masked && !named) leak++;
        }
        i+=1+l;
    }
    return leak;
}
```

- [ ] **Step 3: Wire the report + audit into main** (replace the final `printf`):

```c
    int total_leak=0, worst=0;
    for(size_t i=0;i<count;i++){ int lk=audit_record(&store[i]); total_leak+=lk; if(lk>worst) worst=lk; }
    // reinforce distribution + top-N
    printf("=== pcap-learn report ===\n");
    printf("adverts in       : %u\n", in);
    printf("parsed (stripped): %u\n", ok);
    printf("rejected         : %u  (law3=%u other=%u)\n", rej, rej_law3, rej_other);
    printf("unique shapes    : %zu\n", count);
    printf("structure audit  : %s (%d leaked identity bytes across store, worst rec=%d)\n",
           total_leak? "FAIL":"PASS", total_leak, worst);
    // top 10 by reinforce_count
    printf("top shapes (family/company/svc/ad_len x reinforce):\n");
    for(int n=0;n<10;n++){ int bi=-1; for(size_t i=0;i<count;i++){ if(store[i].reinforce_count==0xFFFF) continue;
            if(bi<0||store[i].reinforce_count>store[bi].reinforce_count) bi=(int)i; }
        if(bi<0) break; learned_template_t *t=&store[bi];
        printf("  fam=%u company=0x%04X svc=0x%04X adlen=%u  x%u\n",
               t->family,t->company_id,t->svc_uuid,t->ad_len,t->reinforce_count);
        store[bi].reinforce_count=0xFFFF; } // mark shown (copy first if you need to keep values)
    return total_leak ? 2 : 0;
```
(Use a shown[] bitmap instead of clobbering `reinforce_count` if T4 needs the real values for the seed — reorder so the seed is written BEFORE the top-N clobber, or snapshot counts first.)

- [ ] **Step 4: Build + run.**
```
make && ./harness adverts.ndjson
```
Expected: full report, `structure audit : PASS (0 leaked ...)`, exit 0. **If it says FAIL, that is a real finding** — investigate whether `learn_strip` under-masks on some real advert shape before proceeding.

- [ ] **Step 5: Commit.**

---

### Task 4: seed emit + self-check + README

**Files:** Modify `tools/pcap_learn/harness.c`; create `tools/pcap_learn/README.md`

**Interfaces:** writes `learn.seed`; re-gates every emitted record.

- [ ] **Step 1: Emit `learn.seed`** BEFORE the top-N clobber. Add after the audit:

```c
    // seed = top-N by reinforce_count (N default 128 = Ward cap); re-gate each first.
    #define SEED_N 128
    static learned_template_t seed[SEED_N]; size_t sn = learn_top_n(store,count,seed,SEED_N);
    int regate_fail=0; for(size_t i=0;i<sn;i++) if(!learn_regate(&seed[i])) regate_fail++;
    printf("seed records     : %zu  (regate_fail=%d)\n", sn, regate_fail);
    FILE *sf=fopen("learn.seed","wb");
    if(sf){ uint32_t magic=0x4C534431u; uint16_t ver=1, cnt=(uint16_t)sn;
        fwrite(&magic,4,1,sf); fwrite(&ver,2,1,sf); fwrite(&cnt,2,1,sf);
        fwrite(seed,sizeof(learned_template_t),sn,sf); fclose(sf);
        printf("wrote learn.seed : %zu B\n", 8+sn*sizeof(learned_template_t)); }
```
`learn_top_n` is declared in `learn_wire.h`. Ensure the audit/top-N-print does not run before this (or snapshot). Fail the run (exit 3) if `regate_fail` > 0 — the device would reject those.

- [ ] **Step 2: Build + run** → confirm `seed records`, `regate_fail=0`, `wrote learn.seed`.

- [ ] **Step 3: README.md** — usage: `python parse_pcap.py <pcap> > adverts.ndjson`, `make`, `./harness adverts.ndjson`, note that `learn.seed` is copied to `/sdcard/simulacra/learn.seed` on a Vigil card, and that pcap/seed are local-only.

- [ ] **Step 4: Commit.**

---

### Task 5: Vigil boot-time seed import

**Files:** Modify `cyd/main/cyd_main.c` (in `app_main`, after `learn_db_load()` and the fleet block)

**Interfaces:** consumes `learn_regate`, `learn_merge_wire` (via `learn_ingest_wire` — which already regates+max-merges into a store), the SD paths.

- [ ] **Step 1: Add the import function** near the learn.db helpers:

```c
#define LEARN_SEED_PATH SD_MOUNT_POINT "/simulacra/learn.seed"
#define LEARN_SEED_DONE SD_MOUNT_POINT "/simulacra/learn.seed.done"
static void learn_seed_import(void){
    FILE *f = fopen(LEARN_SEED_PATH, "rb");
    if(!f) return;
    uint32_t magic=0; uint16_t ver=0, cnt=0;
    if(fread(&magic,4,1,f)!=1 || fread(&ver,2,1,f)!=1 || fread(&cnt,2,1,f)!=1 || magic!=0x4C534431u){
        fclose(f); ESP_LOGW(TAG,"seed: bad header, ignoring"); return; }
    unsigned imported=0, seen=0;
    for(uint16_t i=0;i<cnt;i++){
        learned_template_t rec;
        if(fread(&rec,sizeof rec,1,f)!=1) break;
        seen++;
        if(!learn_regate(&rec)) continue;                 // never trust a seed
        if(learn_merge_wire(s_lib,&s_lib_count,VIGIL_LIB_CAP,&rec,s_lib_sweep)) imported++;
    }
    fclose(f);
    if(imported) s_lib_dirty = true;                       // debounced sealed save persists it
    ESP_LOGW(TAG,"seed: imported %u/%u records -> lib %u", imported, seen, (unsigned)s_lib_count);
    remove(LEARN_SEED_DONE); rename(LEARN_SEED_PATH, LEARN_SEED_DONE);   // one-shot
}
```

- [ ] **Step 2: Call it** right after `learn_db_load(); sig_db_init();` (before the radar loop):
```c
    learn_seed_import();
```

- [ ] **Step 3: Compile-verify** the Vigil build:
```
cd cyd && idf.py -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1 build
```
Expected: builds clean. (HW SD test deferred until hardware is available.)

- [ ] **Step 4: Commit + push + draft PR.**

```
git add tools/pcap_learn cyd/main/cyd_main.c
git commit -m "feat(learn): pcap->learn host harness + Vigil seed import"
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin feat/pcap-learn-harness
gh pr create --draft --base main --head feat/pcap-learn-harness --title "feat(learn): pcap self-learning harness + Vigil seed import" --body "...🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

---

## Self-Review

- **Spec coverage:** parse (T1), real-pipeline compile (T2), report + structure-only audit (T3), seed emit + regate self-check + README (T4), Vigil import (T5). All spec sections mapped.
- **Type consistency:** `learn_strip(ad,len,company,out)`, `learn_shape_hash(t)`, `learn_merge(store,&count,cap,rec,sweep)`, `learn_top_n(store,count,out,n)`, `learn_regate(rec)`, `learn_merge_wire(...)` — all match the headers read. `learned_template_t` PACKED, 55 B, written raw to `learn.seed` and read raw by the firmware (same struct, same packing → portable, as the wire path already relies on).
- **Ordering caveat flagged:** write the seed (T4) BEFORE any top-N print that clobbers `reinforce_count` (T3 Step 3) — snapshot or reorder. Called out in T3/T4.
- **Placeholder scan:** none. The one open contingency (learn.c needing a templates symbol beyond the enum) is handled with an explicit "the compiler names it, add a minimal decl" step.
