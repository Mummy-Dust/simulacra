# Probe-Request Archetype Realism — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the C5 probe injector's minimal IE tail with byte-faithful, per-archetype, per-band 802.11 information-element sets (iPhone / Galaxy / Pixel / generic Android), verified by a host-side reference-fixture test harness.

**Architecture:** Extract the pure frame builder + a new archetype data table into `main/probe_frame.c` (host-compilable; deps `<string.h>`, `<stdint.h>`, `esp_random.h` only). `main/probe.c` keeps the ESP-only radio path and consumes the builder. A new `tools/probe_audit/` host harness (mirroring `tools/decoy_audit/`) compiles `probe_frame.c` and asserts structure + byte-exact fixtures.

**Tech Stack:** C (ESP-IDF 5.5, target esp32c5), host C build via MSVC `cl` / `cc`, Python `unittest` driving a compiled dumper.

## Global Constraints

- **Repo is PUBLIC.** No absolute local paths, no OS username, no real MACs in any committed file. Grep new/changed files for `C:\Users`, `/Users/`, `/home/`, and MAC patterns before each commit.
- **Law 3 (Wi-Fi):** every archetype's SSID IE is id `0x00`, **len 0** (wildcard). Never a real SSID. Source MAC is randomized locally-administered (existing `probe_random_mac`). Vendor IEs carry only capability/type bytes — no identity, no PNL.
- **Independent pool:** archetypes are NOT linked to BLE identities.
- **Commit trailer:** end every commit message with
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
- **Branch:** work on `feat/probe-archetypes` (already created; spec committed there).
- Each archetype tail is **modeled** from documented probe structures for now; mark each with a `// source:` / `# source:` comment. Real captures replace them in a later milestone.

## File Structure

- **Create** `main/probe_frame.h` — `probe_arch_t`, `probe_archetype_t`, `PROBE_FRAME_MAX`, builder + archetype API.
- **Create** `main/probe_frame.c` — archetype IE tables + pure `probe_build_request` / `probe_random_mac` / `probe_pick_archetype`.
- **Modify** `main/probe.c` — drop the pure functions (now in probe_frame); per-phone archetype in pool + rotation; band selection in burst; one-time mix log.
- **Modify** `main/probe.h` — remove the moved decls; keep the live-path decls; `#include "probe_frame.h"`.
- **Modify** `main/CMakeLists.txt` — add `probe_frame.c` to `SRCS`.
- **Create** `tools/probe_audit/` — `Makefile`, `run.ps1`, `probe_dump.c`, `host_stubs/{esp_random.h,portab.h}`, `tests/test_probe_frame.py`, `tests/make_fixtures.py`, `fixtures/*.hex`, `README.md`.

---

### Task 1: Extract pure builder into `probe_frame.{h,c}` (no behavior change)

**Files:**
- Create: `main/probe_frame.h`, `main/probe_frame.c`
- Modify: `main/probe.h`, `main/probe.c:1-35`, `main/CMakeLists.txt:2`

**Interfaces:**
- Produces: `probe_random_mac(uint8_t[6])`, `int probe_build_request(const uint8_t mac[6], uint8_t channel, uint8_t *out, size_t *out_len)`, `#define PROBE_FRAME_MAX 64` — all relocated verbatim, signature unchanged this task.

- [ ] **Step 1: Create `main/probe_frame.h`** with the moved declarations:

```c
#pragma once
#include <stdint.h>
#include <stddef.h>

#define PROBE_FRAME_MAX 64   // raised to 256 in Task 3

void probe_random_mac(uint8_t out[6]);
int  probe_build_request(const uint8_t mac[6], uint8_t channel, uint8_t *out, size_t *out_len);
```

- [ ] **Step 2: Create `main/probe_frame.c`** by moving `probe.c` lines 1–35 verbatim:

```c
#include <string.h>
#include "probe_frame.h"
#include "esp_random.h"

void probe_random_mac(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[0] = (uint8_t)((out[0] & 0xFC) | 0x02);
        int zero = 1, ff = 1;
        for (int i = 0; i < 6; i++) { if (out[i]) zero = 0; if (out[i] != 0xff) ff = 0; }
        if (!zero && !ff) return;
    }
}

int probe_build_request(const uint8_t mac[6], uint8_t channel, uint8_t *out, size_t *out_len)
{
    static const uint8_t hdr_tail[] = {
        0x00, 0x00,
        0x01, 0x04, 0x02, 0x04, 0x0b, 0x16,
        0x32, 0x04, 0x0c, 0x12, 0x18, 0x24,
        0x03, 0x01, 0x00,
    };
    uint8_t *p = out;
    *p++ = 0x40; *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x00;
    memset(p, 0xff, 6); p += 6;
    memcpy(p, mac, 6); p += 6;
    memset(p, 0xff, 6); p += 6;
    *p++ = 0x00; *p++ = 0x00;
    memcpy(p, hdr_tail, sizeof(hdr_tail)); p += sizeof(hdr_tail);
    out[p - out - 1] = channel;
    *out_len = (size_t)(p - out);
    return 0;
}
```

- [ ] **Step 3: Trim `main/probe.c`** — delete moved lines 1–35, replace the top includes with:

```c
#include "probe.h"
#include "probe_frame.h"
```
Keep everything from the old line 37 onward (the `#include "esp_log.h"` … radio path). Remove `#include "esp_random.h"` from probe.c only if now unused (it is used by rotation — keep it).

- [ ] **Step 4: Trim `main/probe.h`** — remove the `PROBE_FRAME_MAX` define and the `probe_random_mac` / `probe_build_request` decls (now in probe_frame.h); add `#include "probe_frame.h"` at the top. Keep all scheduler/live decls (`probe_start`, `probe_wifi_init`, `probe_pool_init`, `probe_inject_burst`, `probe_phone_count`, `probe_total_sent`, `probe_channels_24/5g`).

- [ ] **Step 5: Add to `main/CMakeLists.txt`** — insert `"probe_frame.c"` into the `SRCS` list (after `"probe.c"`).

- [ ] **Step 6: Verify the firmware still builds (C5).** Run:
```
idf.py set-target esp32c5
idf.py -DSIMULACRA_PROBE=1 build
```
Expected: `Project build complete`. (If the workspace is on another target, `rm -rf build sdkconfig` first.)

- [ ] **Step 7: Commit.**
```
git add main/probe_frame.h main/probe_frame.c main/probe.c main/probe.h main/CMakeLists.txt
git commit -m "refactor(probe): extract pure frame builder to probe_frame.{h,c}"
```

---

### Task 2: Stand up `tools/probe_audit/` host harness; pin current builder

**Files:**
- Create: `tools/probe_audit/host_stubs/esp_random.h`, `tools/probe_audit/host_stubs/portab.h`, `tools/probe_audit/probe_dump.c`, `tools/probe_audit/Makefile`, `tools/probe_audit/run.ps1`, `tools/probe_audit/tests/test_probe_frame.py`, `tools/probe_audit/README.md`

**Interfaces:**
- Consumes: `probe_build_request` (current minimal signature) from Task 1.
- Produces: `probe_dump[.exe]` that prints one hex line per requested frame; a python test asserting a valid wildcard probe.

- [ ] **Step 1: Copy the two host stubs** verbatim from `tools/decoy_audit/host_stubs/`:
  - `esp_random.h` (the `rand()`-backed `esp_random`/`esp_fill_random`).
  - `portab.h` (the `/FIportab.h` shim: `#define __attribute__(x)` and `__builtin_popcount`).

- [ ] **Step 2: Write `tools/probe_audit/probe_dump.c`** — build frames for `argv`:

```c
#include <stdio.h>
#include <stdlib.h>
#include "probe_frame.h"

/* Usage: probe_dump <channel>   -> one hex line: the current builder's frame. */
int main(int argc, char **argv) {
    unsigned ch = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 10) : 6;
    uint8_t mac[6] = {0x02,0x11,0x22,0x33,0x44,0x55};   /* fixed for deterministic fixtures */
    uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
    if (probe_build_request(mac, (uint8_t)ch, f, &n)) { fprintf(stderr, "build failed\n"); return 2; }
    for (size_t i = 0; i < n; i++) printf("%02x", f[i]);
    printf("\n");
    return 0;
}
```

- [ ] **Step 3: Write `tools/probe_audit/Makefile`** (mirrors decoy_audit):

```make
CC ?= cc
ROOT := ../..
SRC := probe_dump.c $(ROOT)/main/probe_frame.c
INC := -Ihost_stubs -I$(ROOT)/main
CFLAGS ?= -O2 -Wall -Wno-unused-parameter
all: probe_dump
probe_dump: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o probe_dump
clean:
	rm -f probe_dump probe_dump.exe
```

- [ ] **Step 4: Write `tools/probe_audit/run.ps1`** — build the dumper (MSVC), then run the tests:

```powershell
[CmdletBinding()] param([switch]$Rebuild)
$ErrorActionPreference = "Continue"
$tool = $PSScriptRoot
$exe  = Join-Path $tool "probe_dump.exe"
if ($Rebuild -or -not (Test-Path $exe)) {
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        Write-Error "cl (MSVC) not on PATH. Open 'Developer PowerShell for VS' first."; exit 3
    }
    Push-Location $tool
    try {
        cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h `
           /Ihost_stubs /I..\..\main `
           probe_dump.c ..\..\main\probe_frame.c /Fe:probe_dump.exe | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 3 }
    } finally { Pop-Location }
}
python -m unittest discover -s (Join-Path $tool "tests") -v
exit $LASTEXITCODE
```

- [ ] **Step 5: Write the failing test `tools/probe_audit/tests/test_probe_frame.py`:**

```python
import os, subprocess, unittest
HERE = os.path.dirname(__file__)
TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")

def build(channel):
    out = subprocess.check_output([EXE, str(channel)], text=True).strip()
    return bytes.fromhex(out)

def ies(frame):
    """Return {id: value_bytes} walking the IE body after the 24-byte header + 2 seq."""
    body, i, out = frame[24:], 0, {}
    while i + 2 <= len(body):
        eid, ln = body[i], body[i+1]
        out.setdefault(eid, body[i+2:i+2+ln])
        i += 2 + ln
    return out

@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class ProbeFrame(unittest.TestCase):
    def test_valid_wildcard_probe(self):
        f = build(6)
        self.assertEqual(f[0:2], b"\x40\x00")              # FC: probe request
        self.assertEqual(f[4:10], b"\xff"*6)               # DA broadcast
        self.assertEqual(f[16:22], b"\xff"*6)              # BSSID broadcast
        d = ies(f)
        self.assertIn(0x00, d)                             # SSID present
        self.assertEqual(len(d[0x00]), 0)                  # wildcard (Law 3)
        self.assertEqual(d[0x03], bytes([6]))              # DS Param == requested channel

if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 6: Build and run — verify it PASSES** (the current minimal builder is already a valid wildcard probe):
```
pwsh tools/probe_audit/run.ps1 -Rebuild
```
Expected: `test_valid_wildcard_probe ... ok`.

- [ ] **Step 7: Write `tools/probe_audit/README.md`** — one paragraph: what it does (host-builds `probe_frame.c`, asserts probe frames are valid wildcard probes and match reference fixtures), how to run (`run.ps1 -Rebuild`), and that fixtures are regenerated with `tests/make_fixtures.py` (added Task 3). Add a `.gitignore` line or note to not commit `probe_dump.exe`/`*.obj`.

- [ ] **Step 8: Commit.**
```
git add tools/probe_audit
git commit -m "test(probe): host harness pinning the probe builder as a valid wildcard probe"
```

---

### Task 3: Archetype-aware builder + iPhone archetype (2.4 + 5 GHz)

**Files:**
- Modify: `main/probe_frame.h`, `main/probe_frame.c`, `tools/probe_audit/probe_dump.c`, `tools/probe_audit/tests/test_probe_frame.py`
- Create: `tools/probe_audit/tests/make_fixtures.py`, `tools/probe_audit/fixtures/iphone_24.hex`, `tools/probe_audit/fixtures/iphone_5.hex`

**Interfaces:**
- Produces:
  - `typedef enum { ARCH_IPHONE, ARCH_GALAXY, ARCH_PIXEL, ARCH_ANDROID, PROBE_ARCH_COUNT } probe_arch_t;`
  - `int probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5, uint8_t *out, size_t *out_len);`
  - `const probe_archetype_t *probe_archetype(probe_arch_t);` `size_t probe_archetype_count(void);` `probe_arch_t probe_pick_archetype(void);`
  - `#define PROBE_FRAME_MAX 256`

- [ ] **Step 1: Rewrite `main/probe_frame.h`:**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PROBE_FRAME_MAX 256   // rich HT/VHT/HE/ext-cap/vendor IEs exceed 64

typedef enum { ARCH_IPHONE, ARCH_GALAXY, ARCH_PIXEL, ARCH_ANDROID, PROBE_ARCH_COUNT } probe_arch_t;

typedef struct {
    const char    *name;
    const uint8_t *tail24; uint16_t tail24_len; int16_t ds_off24;  // -1 if band absent
    const uint8_t *tail5;  uint16_t tail5_len;  int16_t ds_off5;
    uint8_t        weight;                                          // fixed pool-draw spread
} probe_archetype_t;

void   probe_random_mac(uint8_t out[6]);
const  probe_archetype_t *probe_archetype(probe_arch_t a);
size_t probe_archetype_count(void);
probe_arch_t probe_pick_archetype(void);                           // weighted (uses esp_random)

// Build a wildcard probe request for `mac` on `ch` using `arch`'s per-band IE set.
// band5 selects the 5 GHz tail. Returns 0 on success; non-zero if arch lacks that band.
int probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                        uint8_t *out, size_t *out_len);
```

- [ ] **Step 2: In `main/probe_frame.c`, add the iPhone IE tails** (modeled — replace with capture later). Each `ds_off` is the 0-based offset **within the tail** of the DS-Param channel byte.

```c
// source: modeled from a documented iOS probe request (2.4 GHz). WILDCARD SSID (Law 3).
// SSID(2) Rates(10) ExtRates(6) DS(3) HTcap(28) ExtCap(10) HEcap(24) AppleVendor(12)
static const uint8_t IPHONE_24[] = {
    0x00,0x00,                                             // SSID wildcard
    0x01,0x08,0x82,0x84,0x8b,0x96,0x0c,0x12,0x18,0x24,     // Supported Rates (CCK basic + OFDM)
    0x32,0x04,0x30,0x48,0x60,0x6c,                         // Extended Rates 24/36/48/54
    0x03,0x01,0x00,                                        // DS Param (channel patched)  <- ds byte
    0x2d,0x1a,0xad,0x01,0x17,0xff,0xff,0xff,0x00,0x00,     // HT Capabilities (26 bytes)
              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
              0x00,0x00,0x00,0x00,0x00,0x00,
    0x7f,0x08,0x04,0x00,0x08,0x84,0x00,0x00,0x00,0x40,     // Extended Capabilities
    0xff,0x16,0x23,0x09,0x01,0x08,0x1a,0x40,0x00,0x04,     // HE Capabilities (ext id 0x23)
              0x70,0x0c,0x89,0x7c,0xc8,0x07,0xcc,0xcc,0xcc,0x00,0x00,0x00,
    0xdd,0x0a,0x00,0x17,0xf2,0x0a,0x00,0x01,0x04,0x00,0x00,0x00,  // Apple vendor IE (00-17-F2)
};
#define IPHONE_24_DS 20   // offset of the DS channel byte within IPHONE_24

// source: modeled from a documented iOS probe request (5 GHz). No CCK rates.
// SSID(2) Rates(10) DS(3) HTcap(28) VHTcap(14) HEcap(24) AppleVendor(12)
static const uint8_t IPHONE_5[] = {
    0x00,0x00,
    0x01,0x08,0x8c,0x12,0x98,0x24,0xb0,0x48,0x60,0x6c,     // OFDM rates 6..54 (some basic)
    0x03,0x01,0x00,                                        // DS Param  <- ds byte at offset 12
    0x2d,0x1a,0xad,0x01,0x17,0xff,0xff,0xff,0x00,0x00,
              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
              0x00,0x00,0x00,0x00,0x00,0x00,
    0xbf,0x0c,0x92,0x71,0x80,0x0f,0xea,0xff,0x00,0x00,0xea,0xff,0x00,0x00,  // VHT Capabilities
    0xff,0x16,0x23,0x09,0x01,0x08,0x1a,0x40,0x00,0x04,
              0x70,0x0c,0x89,0x7c,0xc8,0x07,0xcc,0xcc,0xcc,0x00,0x00,0x00,
    0xdd,0x0a,0x00,0x17,0xf2,0x0a,0x00,0x01,0x04,0x00,0x00,0x00,
};
#define IPHONE_5_DS 12
```

- [ ] **Step 3: Add the archetype table + accessors** (only iPhone populated this task; others get `{0}` placeholders filled in Task 4). Use a temporary table with just iPhone so the build is valid:

```c
static const probe_archetype_t ARCHS[PROBE_ARCH_COUNT] = {
    [ARCH_IPHONE] = { "iphone",
        IPHONE_24, sizeof IPHONE_24, IPHONE_24_DS,
        IPHONE_5,  sizeof IPHONE_5,  IPHONE_5_DS,  40 },
    // ARCH_GALAXY / ARCH_PIXEL / ARCH_ANDROID filled in Task 4
};

const probe_archetype_t *probe_archetype(probe_arch_t a) {
    return (a < PROBE_ARCH_COUNT) ? &ARCHS[a] : 0;
}
size_t probe_archetype_count(void) { return PROBE_ARCH_COUNT; }

probe_arch_t probe_pick_archetype(void) {
    uint32_t total = 0;
    for (size_t i = 0; i < PROBE_ARCH_COUNT; i++) total += ARCHS[i].weight;
    if (!total) return ARCH_IPHONE;
    uint32_t r = esp_random() % total;
    for (size_t i = 0; i < PROBE_ARCH_COUNT; i++) {
        if (r < ARCHS[i].weight) return (probe_arch_t)i;
        r -= ARCHS[i].weight;
    }
    return ARCH_IPHONE;
}
```
Add `#include "esp_random.h"` (already present) and `#include <stdbool.h>`.

- [ ] **Step 4: Replace `probe_build_request`** with the archetype-aware version:

```c
int probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                        uint8_t *out, size_t *out_len)
{
    const probe_archetype_t *a = probe_archetype(arch);
    if (!a) return 1;
    const uint8_t *tail = band5 ? a->tail5 : a->tail24;
    uint16_t tlen       = band5 ? a->tail5_len : a->tail24_len;
    int16_t  ds_off     = band5 ? a->ds_off5 : a->ds_off24;
    if (!tail || tlen == 0) return 2;                      // archetype lacks this band
    if (24u + 2u + tlen > PROBE_FRAME_MAX) return 3;

    uint8_t *p = out;
    *p++ = 0x40; *p++ = 0x00;                              // FC: probe request
    *p++ = 0x00; *p++ = 0x00;                              // duration
    memset(p, 0xff, 6); p += 6;                            // DA broadcast
    memcpy(p, mac, 6); p += 6;                             // SA
    memset(p, 0xff, 6); p += 6;                            // BSSID broadcast
    *p++ = 0x00; *p++ = 0x00;                              // seq (driver overwrites)
    memcpy(p, tail, tlen); p += tlen;
    if (ds_off >= 0) out[24 + 2 + ds_off] = ch;            // patch DS channel
    *out_len = (size_t)(p - out);
    return 0;
}
```

- [ ] **Step 5: Update `tools/probe_audit/probe_dump.c`** to take `<arch> <channel> <band5>`:

```c
#include <stdio.h>
#include <stdlib.h>
#include "probe_frame.h"
/* Usage: probe_dump <arch_idx> <channel> <band5:0|1> */
int main(int argc, char **argv) {
    probe_arch_t a = (argc > 1) ? (probe_arch_t)strtoul(argv[1],0,10) : ARCH_IPHONE;
    unsigned ch    = (argc > 2) ? (unsigned)strtoul(argv[2],0,10) : 6;
    bool band5     = (argc > 3) ? (strtoul(argv[3],0,10) != 0) : false;
    uint8_t mac[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
    uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
    if (probe_build_request(mac, (uint8_t)ch, a, band5, f, &n)) { fprintf(stderr,"build failed\n"); return 2; }
    for (size_t i = 0; i < n; i++) printf("%02x", f[i]);
    printf("\n");
    return 0;
}
```

- [ ] **Step 6: Write `tools/probe_audit/tests/make_fixtures.py`** — regenerates `fixtures/<arch>_<band>.hex` from the built dumper (one-shot helper, like decoy_audit's):

```python
import os, subprocess
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")
FIX  = os.path.join(TOOL, "fixtures"); os.makedirs(FIX, exist_ok=True)
# (arch_idx, name, channel, band5)
CASES = [(0,"iphone",6,0),(0,"iphone",36,1)]
for idx,name,ch,b5 in CASES:
    hexline = subprocess.check_output([EXE,str(idx),str(ch),str(b5)], text=True).strip()
    tag = "5" if b5 else "24"
    with open(os.path.join(FIX, f"{name}_{tag}.hex"),"w") as fh:
        fh.write("# source: modeled probe request; replace with real capture later\n")
        fh.write(hexline + "\n")
    print("wrote", f"{name}_{tag}.hex")
```

- [ ] **Step 7: Extend the test** with structural asserts + byte-exact fixture compare. Append to `test_probe_frame.py`:

```python
def build_arch(idx, ch, b5):
    out = subprocess.check_output([EXE, str(idx), str(ch), str(b5)], text=True).strip()
    return bytes.fromhex(out)

def fixture(name):
    p = os.path.join(TOOL, "fixtures", name)
    with open(p) as fh:
        return bytes.fromhex([l for l in fh if not l.startswith("#")][0].strip())

@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Archetypes(unittest.TestCase):
    CASES = [("iphone", 0, 6, 0, "iphone_24.hex"), ("iphone", 0, 36, 1, "iphone_5.hex")]
    def test_structure_and_law3(self):
        for name, idx, ch, b5, _ in self.CASES:
            f = build_arch(idx, ch, b5)
            self.assertEqual(f[0:2], b"\x40\x00", name)
            self.assertEqual(f[4:10], b"\xff"*6, name)
            self.assertEqual(f[16:22], b"\xff"*6, name)
            d = ies(f)
            self.assertIn(0x00, d, name)
            self.assertEqual(len(d[0x00]), 0, f"{name} SSID must be wildcard (Law 3)")
            self.assertEqual(d[0x03], bytes([ch]), f"{name} DS channel")
            self.assertLessEqual(len(f), 256, f"{name} within PROBE_FRAME_MAX")
            self.assertIn(0x2d, d, f"{name} HT caps present")
            if b5: self.assertIn(0xbf, d, f"{name} VHT caps on 5 GHz")
    def test_matches_fixture(self):
        for name, idx, ch, b5, fx in self.CASES:
            self.assertEqual(build_arch(idx, ch, b5), fixture(fx), f"{name} byte-exact fixture")
```

- [ ] **Step 8: Generate fixtures, then run the tests:**
```
pwsh tools/probe_audit/run.ps1 -Rebuild
python tools/probe_audit/tests/make_fixtures.py
python -m unittest discover -s tools/probe_audit/tests -v
```
Expected: `test_structure_and_law3 ... ok`, `test_matches_fixture ... ok`, and the Task-2 `test_valid_wildcard_probe` updated or removed (it used the old 1-arg dumper — update it to call `build_arch(0,6,0)` or delete it in favor of `test_structure_and_law3`).

- [ ] **Step 9: Verify firmware builds** with the new signature (probe.c still calls the OLD signature — this build is EXPECTED to fail until Task 5). Instead, verify only the host harness here; note in the commit that probe.c wiring lands in Task 5. To keep the tree buildable between tasks, add a temporary shim in probe.c: replace its `probe_build_request(s_macs[i], channel, f, &n)` call site with `probe_build_request(s_macs[i], channel, ARCH_IPHONE, channel>=36, f, &n)`. Rebuild C5: `idf.py -DSIMULACRA_PROBE=1 build` → `Project build complete`.

- [ ] **Step 10: Commit.**
```
git add main/probe_frame.h main/probe_frame.c main/probe.c tools/probe_audit
git commit -m "feat(probe): archetype-aware builder + iPhone 2.4/5 GHz IE tails + host tests"
```

---

### Task 4: Add Galaxy, Pixel, generic Android archetypes

**Files:**
- Modify: `main/probe_frame.c` (three more tail pairs + table entries), `tools/probe_audit/tests/make_fixtures.py` (add cases), `tools/probe_audit/tests/test_probe_frame.py` (add cases)
- Create: `tools/probe_audit/fixtures/{galaxy,pixel,android}_{24,5}.hex`

**Interfaces:**
- Consumes: the Task 3 archetype table + builder.
- Produces: fully-populated `ARCHS[PROBE_ARCH_COUNT]`; a weighted-draw distribution the pool relies on in Task 5.

- [ ] **Step 1: Add `GALAXY_24/GALAXY_5`, `PIXEL_24/PIXEL_5`, `ANDROID_24/ANDROID_5`** to `probe_frame.c`, each a valid wildcard-SSID tail with its own capability bytes + vendor IE, and a matching `#define <NAME>_DS <offset>`. Differentiate them so the crowd is diverse (each is `// source: modeled …`):
  - **Galaxy** (Samsung/Broadcom): HT + VHT + HE; vendor IE Broadcom `00-10-18` on 2.4, plus WFA `00-50-f2` WMM. Distinct HT/VHT capability bytes from iPhone.
  - **Pixel** (Google/Qualcomm): HT + VHT + HE; vendor IE `00-50-f2` (WFA) + Google-style ext-cap byte differences.
  - **Android (generic)**: HT only on 2.4 (no HE), HT + VHT on 5 GHz; smaller ext-cap; vendor IE `00-50-f2` WPS-style. Represents older/low-end devices.

  Use the iPhone tails as the byte-layout template; change: Supported/Extended Rates ordering, HT cap info bytes (offsets 2–3 of the HT IE), presence/absence of VHT/HE, ext-cap payload, and the vendor OUI/type. Keep every `id,len` self-consistent (len == number of data bytes). Keep SSID `00 00`.

  > No placeholder bytes: author each array fully, then let `make_fixtures.py` pin them. The **structural** test (below) is the real correctness gate; the fixture guards against regression.

- [ ] **Step 2: Fill the table entries** in `ARCHS`:

```c
    [ARCH_GALAXY]  = { "galaxy", GALAXY_24, sizeof GALAXY_24, GALAXY_24_DS,
                       GALAXY_5,  sizeof GALAXY_5,  GALAXY_5_DS,  25 },
    [ARCH_PIXEL]   = { "pixel",  PIXEL_24,  sizeof PIXEL_24,  PIXEL_24_DS,
                       PIXEL_5,   sizeof PIXEL_5,   PIXEL_5_DS,   15 },
    [ARCH_ANDROID] = { "android",ANDROID_24,sizeof ANDROID_24,ANDROID_24_DS,
                       ANDROID_5, sizeof ANDROID_5, ANDROID_5_DS, 20 },
```

- [ ] **Step 3: Extend `make_fixtures.py` `CASES`** with the six new (idx,name,ch,band) rows (galaxy=1, pixel=2, android=3; 2.4 ch 6, 5 GHz ch 36).

- [ ] **Step 4: Extend the test `CASES`** in `Archetypes` with all four archetypes × both bands (8 rows). The generic Android 2.4 case asserts HT present but **not** HE (`self.assertNotIn(0xff, d)` for that specific case) to prove archetype diversity is real.

- [ ] **Step 5: Add a weighted-distribution test** for `probe_pick_archetype`. Add a `--pick N` mode to `probe_dump.c` that prints N archetype indices (one per line) using `probe_pick_archetype()`, seeded by `srand(argv seed)`:

```c
// in main(), before the build path:
if (argc > 1 && strcmp(argv[1], "--pick") == 0) {
    srand(argc > 2 ? (unsigned)strtoul(argv[2],0,10) : 1);
    int n = argc > 3 ? (int)strtoul(argv[3],0,10) : 1000;
    for (int i = 0; i < n; i++) printf("%d\n", (int)probe_pick_archetype());
    return 0;
}
```
Test: run `probe_dump --pick 1 4000`, count indices, assert iPhone is the plurality and every archetype appears (weights 40/25/15/20 → each share within a tolerance band, e.g. iPhone in 0.33–0.47, android in 0.13–0.27).

- [ ] **Step 6: Regenerate fixtures and run the full suite:**
```
pwsh tools/probe_audit/run.ps1 -Rebuild
python tools/probe_audit/tests/make_fixtures.py
python -m unittest discover -s tools/probe_audit/tests -v
```
Expected: all structural, fixture, and distribution tests pass; 8 fixtures present.

- [ ] **Step 7: Commit.**
```
git add main/probe_frame.c tools/probe_audit
git commit -m "feat(probe): add Galaxy/Pixel/generic-Android archetypes + weighted-pick test"
```

---

### Task 5: Wire archetypes into the live pool, rotation, and burst

**Files:**
- Modify: `main/probe.c:65-119` (pool storage, `probe_pool_init`, `probe_inject_burst`, rotation), and the dev-mode `next_channel`/`probe_task` if needed.

**Interfaces:**
- Consumes: `probe_pick_archetype`, `probe_build_request(mac, ch, arch, band5, …)`, `probe_archetype`.
- Produces: no new external API; behavior change only. Verified on-target by a clean C5 build (host tests already cover frame correctness).

- [ ] **Step 1: Change the pool storage** in `probe.c`. Replace `static uint8_t s_macs[PROBE_MAX_PHONES][6];` with:

```c
typedef struct { uint8_t mac[6]; probe_arch_t arch; } probe_phone_t;
static probe_phone_t s_phones[PROBE_MAX_PHONES];
```
Update `s_n` usage to index `s_phones`.

- [ ] **Step 2: Update `probe_pool_init`** to draw an archetype per phone and log the mix:

```c
void probe_pool_init(void)
{
    s_n = PROBE_PHONES; if (s_n > PROBE_MAX_PHONES) s_n = PROBE_MAX_PHONES;
    int mix[PROBE_ARCH_COUNT] = {0};
    for (int i = 0; i < s_n; i++) {
        probe_random_mac(s_phones[i].mac);
        s_phones[i].arch = probe_pick_archetype();
        mix[s_phones[i].arch]++;
    }
    ESP_LOGW(TAG, "probe pool: %d phones (iphone=%d galaxy=%d pixel=%d android=%d)",
             s_n, mix[ARCH_IPHONE], mix[ARCH_GALAXY], mix[ARCH_PIXEL], mix[ARCH_ANDROID]);
}
```

- [ ] **Step 3: Update `probe_inject_burst`** to build per-phone archetype frames with band from channel:

```c
int probe_inject_burst(uint8_t channel)
{
    bool band5 = (channel >= 36);
    int crc = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    int rc = 0;
    for (int i = 0; i < s_n; i++) {
        uint8_t f[PROBE_FRAME_MAX]; size_t n = 0;
        if (probe_build_request(s_phones[i].mac, channel, s_phones[i].arch, band5, f, &n) != 0)
            continue;                                     // archetype lacks this band (defensive)
        rc = esp_wifi_80211_tx(WIFI_IF_STA, f, (int)n, true);
        s_probes_sent++;
    }
    if ((esp_random() % PROBE_ROTATE_EVERY) == 0) {       // retire one phone -> fresh MAC + arch
        int k = esp_random() % s_n;
        probe_random_mac(s_phones[k].mac);
        s_phones[k].arch = probe_pick_archetype();
    }
    ESP_LOGW(TAG, "burst ch=%u phones=%d band5=%d set_ch_rc=%d tx_rc=%d",
             channel, s_n, band5, crc, rc);
    return rc;
}
```

- [ ] **Step 4: Remove the Task-3 temporary shim** if any other call site remains, and delete the now-unused `s_macs`. Ensure `#include "probe_frame.h"` is present (via probe.h).

- [ ] **Step 5: Build the C5 firmware:**
```
idf.py set-target esp32c5
idf.py -DSIMULACRA_PROBE=1 build
```
Expected: `Project build complete`, no warnings about `s_macs`/unused.

- [ ] **Step 6: (Optional on-target) Flash + observe** via the build-flash-read skill and confirm the new logs:
```
& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Port <C5_PORT> -Do all -ReadSeconds 12 -Grep 'probe pool|burst ch'
```
Expected: a `probe pool: N phones (iphone=… …)` line and `burst ch=… band5=…` lines. (The `-Do all` build won't pass `-DSIMULACRA_PROBE`; for a Wi-Fi-only bring-up build with raw `idf.py` instead, or rely on the combined-decoy coexist path which already calls `probe_inject_burst`.)

- [ ] **Step 7: Run the full host suite once more** (nothing should have changed there, but confirm green):
```
python -m unittest discover -s tools/probe_audit/tests -v
```

- [ ] **Step 8: Commit.**
```
git add main/probe.c
git commit -m "feat(probe): per-phone archetype in pool/rotation + band-aware bursts"
```

---

## Self-Review Notes (author)

- **Spec coverage:** archetype library (T3/T4), pure builder extraction + host testability (T1/T2), per-band tails + DS patch (T3), `PROBE_FRAME_MAX`→256 (T3), pool/rotation/band wiring (T5), Law-3 wildcard guard (T2/T3 test), byte-exact fixtures (T3/T4), independent pool (T5, no BLE refs). All present.
- **Type consistency:** `probe_build_request` signature identical in `probe_frame.h`, `probe_frame.c`, `probe_dump.c`, and the `probe.c` call site. `probe_arch_t` enum order matches `ARCHS[]` designated initializers and the dumper's `arch_idx` (0=iphone…3=android).
- **Inter-task buildability:** T3 adds a temporary shim at the probe.c call site so the tree stays buildable; T5 removes it. Note this explicitly so a task-by-task executor doesn't leave a broken build.
- **Modeled bytes honesty:** every archetype tail is `// source: modeled`; the structural test (FC/broadcast/wildcard/DS/HT/VHT presence) is the correctness gate, the fixture is the regression pin, and real captures replace the tails later without touching the tests' structure.
