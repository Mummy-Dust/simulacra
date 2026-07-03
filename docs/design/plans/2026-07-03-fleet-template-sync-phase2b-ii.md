# Fleet Template Sync — Phase 2b-ii (Vigil SD durability) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give Vigil a persistent, encrypted-at-rest SD-card library so the fleet's learned shapes survive reboots and card theft, and let Vigil broadcast the top-N most-established shapes (not just whatever fits RAM).

**Architecture:** Add a shared, host-testable crypto layer (`learn_db.{h,c}` in `components/simulacra_radar/`): HKDF-SHA256 derives a dedicated at-rest key from the fleet PSK, and AES-256-GCM seals/opens the whole library blob with a random per-write nonce. Vigil (`cyd_main.c`) mounts a microSD on its own SPI host (SPI3, separate from the display's SPI2), loads `learn.db` at boot into the existing RAM `s_lib`, and rewrites it atomically (`.tmp` + rename) after merges. `broadcast_library()` gains a top-N-by-`reinforce_count` selection capped to the smallest decoy store. Everything degrades gracefully: no card / bad tag → Vigil runs exactly as it does today (RAM-only librarian + display).

**Tech Stack:** C (ESP-IDF), mbedtls (`mbedtls_hkdf`, `mbedtls_gcm_*` — already a dependency), `esp_vfs_fat_sdspi_mount` (fatfs + sdmmc, IDF core), the shared `simulacra_radar` component, on-target selftest + two-board bench verification.

## Global Constraints

- **Reuse the existing crypto dependency** — mbedtls is already in `simulacra_radar`'s REQUIRES and `cyd/main`'s REQUIRES; no new managed component.
- **At-rest key is distinct from the session key** — HKDF-SHA256 from `SIMULACRA_ESPNOW_KEY` under a dedicated context label `"simulacra-learndb-v1"`, never the raw PSK.
- **Never trust the card** — a failed GCM tag (corrupt / tampered / foreign card) is treated as "no library" and rebuilt from the next sync. No plaintext export.
- **Graceful degradation** — SD absent / unmountable / write-fail must never break the display or the RAM sync path already shipped in Phase 2b-i.
- **Shared component compiles for both targets** — `learn_db.c` must build for the decoy (C5, IDF 5.5) and Vigil (classic ESP32, IDF 5.4), even though only Vigil links the SD I/O. Keep `learn_db.c` pure crypto/serialization (no `esp_vfs_fat` calls); SD file I/O lives in `cyd_main.c`.
- **Idempotent by shape_hash** carries over from Phase 2a/2b-i — merges and reloads must not corrupt or inflate state.

### Design decisions (refinements over the Phase 2 spec — flagged for review)

- **E1 — dirty-bit dropped.** The spec's per-record dirty-bit downlink-confirmation is obsolete: Phase 2b-i chose full-library idempotent sync (D1), so there is nothing to track. The spec's "dirty-bit" test is intentionally not implemented.
- **E2 — SD on its own SPI host (SPI3).** The display owns SPI2 (pins 13/14/15). The classic CYD wires the microSD to SPI3 (CS=5, MOSI=23, SCK=18, MISO=19). Using a separate host sidesteps the spec's "shared SPI subsystem" DMA-contention caveat entirely. Task 1 confirms on the bench; if this board shares the bus, fall back to attaching the SD device to SPI2 (documented in Task 1).
- **E3 — save is debounced, not per-merge.** Rewriting the blob on every received offer would thrash the card. Save at most once per `LEARN_DB_SAVE_MS` (30 s) when the library changed, plus once on the first mount.
- **E4 — top-N reuses `lw_weakest` semantics.** `LEARN_SYNC_TOP_N` defaults to `64` (the Shade/C6 cap, the smallest decoy store) so a full-down always fits every decoy.

### Build / flash / read commands

Decoy (C5, COM12), selftest (IDF 5.5) — Tasks 2/3/5 host logic runs here:
```powershell
$env:PATH = "C:\Program Files\Python312;C:\Program Files\Python312\Scripts;$env:PATH"
$env:IDF_PATH = "~\esp\v5.5\esp-idf"; . "$env:IDF_PATH\export.ps1"
Set-Location ~\Downloads\splinter
idf.py -B build.c5 -DCHURN_SELFTEST=1 build
idf.py -B build.c5 -p COM12 flash
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM12 --seconds 8 --reset yes --grep "SELFTEST|FAIL"
```
Vigil (CYD, COM10), classic ESP32 (IDF 5.4, separate shell) — Tasks 1/4/6:
```powershell
$env:IDF_PATH = "~\esp\v5.4\esp-idf"; . "$env:IDF_PATH\export.ps1"
Set-Location ~\Downloads\splinter\cyd
idf.py build ; idf.py -p COM10 flash
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM10 --seconds 30 --reset yes --grep "sd:|learndb|offer rx|broadcast"
```

---

### Task 1: SD mount spike on Vigil (de-risk the bus)

Prove the CYD can mount a microSD on SPI3 alongside the live display, and that absence degrades gracefully. This is a spike whose code becomes the mount helper Task 4 uses.

**Files:**
- Modify: `cyd/main/cyd_main.c`
- Modify: `cyd/main/CMakeLists.txt`

**Interfaces:**
- Produces: `bool sd_mount(void);` — mounts the card at `SD_MOUNT_POINT`, returns true on success; logs `sd: mounted`/`sd: absent`. `static bool s_sd_ok;` records the result for later tasks.

- [ ] **Step 1: Add the fatfs/sdmmc deps.** In `cyd/main/CMakeLists.txt`, append `fatfs sdmmc` to `REQUIRES`:

```cmake
    REQUIRES simulacra_radar esp_lcd esp_wifi esp_netif esp_event nvs_flash mbedtls driver esp_timer fatfs sdmmc
```

- [ ] **Step 2: Add the mount helper.** In `cyd/main/cyd_main.c`, add includes near the top:

```c
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
```

and the SD pins + helper (after the display pin defines):

```c
// microSD on its own SPI host (SPI3), separate from the display's SPI2 (E2).
#define SD_HOST     SPI3_HOST
#define PIN_SD_MOSI 23
#define PIN_SD_MISO 19
#define PIN_SD_SCK  18
#define PIN_SD_CS    5
#define SD_MOUNT_POINT "/sdcard"
static bool s_sd_ok;
static sdmmc_card_t *s_card;

static bool sd_mount(void)
{
    spi_bus_config_t bus = { .mosi_io_num=PIN_SD_MOSI, .miso_io_num=PIN_SD_MISO,
        .sclk_io_num=PIN_SD_SCK, .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=4096 };
    if (spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGW(TAG,"sd: bus init fail"); return false; }
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = PIN_SD_CS; dev.host_id = SD_HOST;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT(); host.slot = SD_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mnt = { .format_if_mount_failed=false, .max_files=4,
        .allocation_unit_size=16*1024 };
    esp_err_t e = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev, &mnt, &s_card);
    if (e != ESP_OK) { ESP_LOGW(TAG, "sd: absent/unmountable (0x%x) -> RAM-only librarian", e); return false; }
    ESP_LOGW(TAG, "sd: mounted (%lluMB)", ((uint64_t)s_card->csd.capacity)*s_card->csd.sector_size/(1024*1024));
    return true;
}
```

- [ ] **Step 3: Call it at boot + probe read/write.** In `app_main`, after `net_init();`, add:

```c
    s_sd_ok = sd_mount();
    if (s_sd_ok) {                                   // one-shot probe: mkdir + write + read-back
        mkdir(SD_MOUNT_POINT "/simulacra", 0777);
        FILE *f = fopen(SD_MOUNT_POINT "/simulacra/probe.txt", "w");
        if (f) { fputs("ok", f); fclose(f); ESP_LOGW(TAG, "sd: probe write ok"); }
        else ESP_LOGW(TAG, "sd: probe write FAILED");
    }
```

Add `#include <sys/stat.h>` and `#include <stdio.h>` near the includes.

- [ ] **Step 4: Build + flash + read.** `idf.py build` then flash COM10:

```powershell
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM10 --seconds 20 --reset yes --grep "sd:|panel up|espnow up"
```
Expected with a card inserted: `sd: mounted (…MB)` then `sd: probe write ok`, and the radar still comes up (`espnow up`, `panel up`). Expected with no card: `sd: absent/unmountable` and everything else still runs. **Either outcome confirms graceful behavior; a card is required only to exercise the write path.**

> Bench decision: if `sd: bus init fail` or the display breaks, this board shares the bus — attach the SD device to `SPI2_HOST` instead (reuse the display's already-initialized bus; do **not** re-`spi_bus_initialize`), and set `dev.gpio_cs = PIN_SD_CS` on a free CS pin. Record which path worked in the commit message.

- [ ] **Step 5: Commit**

```bash
git add cyd/main/cyd_main.c cyd/main/CMakeLists.txt
git commit -m "feat(vigil): microSD mount on SPI3 + boot probe (graceful when absent)"
```

---

### Task 2: HKDF at-rest key derivation

Derive the SD encryption key from the fleet PSK — host-testable, deterministic.

**Files:**
- Create: `components/simulacra_radar/learn_db.h`
- Create: `components/simulacra_radar/learn_db.c`
- Modify: `components/simulacra_radar/CMakeLists.txt`
- Modify: `main/churn_selftest.c` (`test_learn_db_key()`)

**Interfaces:**
- Produces: `void learn_db_derive_key(const uint8_t psk[32], uint8_t out_key[32]);` — HKDF-SHA256(ikm=psk, salt=NULL, info="simulacra-learndb-v1") → 32-byte key. Deterministic for a given PSK.

- [ ] **Step 1: Write the failing test** in `churn_selftest.c` (call `test_learn_db_key();` after `test_learn_snapshot_ingest();`), and add `#include "learn_db.h"` near the other radar includes:

```c
static void test_learn_db_key(void)
{
    uint8_t k1[32], k2[32];
    learn_db_derive_key(SIMULACRA_ESPNOW_KEY, k1);
    learn_db_derive_key(SIMULACRA_ESPNOW_KEY, k2);
    ST_CHECK(memcmp(k1, k2, 32) == 0, "db key: deterministic");
    ST_CHECK(memcmp(k1, SIMULACRA_ESPNOW_KEY, 32) != 0, "db key: distinct from session PSK");
    uint8_t other[32]; memcpy(other, SIMULACRA_ESPNOW_KEY, 32); other[0] ^= 0xFF;
    uint8_t k3[32]; learn_db_derive_key(other, k3);
    ST_CHECK(memcmp(k1, k3, 32) != 0, "db key: depends on PSK");
}
```

- [ ] **Step 2: Build to verify it fails** — `idf.py -B build.c5 -DCHURN_SELFTEST=1 build`, expect `undefined reference to 'learn_db_derive_key'`.

- [ ] **Step 3: Create `components/simulacra_radar/learn_db.h`:**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "learn_record.h"

#define LEARN_DB_LABEL "simulacra-learndb-v1"

// HKDF-SHA256(ikm=psk[32], salt=NULL, info=LEARN_DB_LABEL) -> out_key[32].
void learn_db_derive_key(const uint8_t psk[32], uint8_t out_key[32]);
```

- [ ] **Step 4: Create `components/simulacra_radar/learn_db.c`:**

```c
#include <string.h>
#include "learn_db.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

void learn_db_derive_key(const uint8_t psk[32], uint8_t out_key[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_hkdf(md, NULL, 0, psk, 32,
                 (const uint8_t *)LEARN_DB_LABEL, strlen(LEARN_DB_LABEL),
                 out_key, 32);
}
```

- [ ] **Step 5: Register the source.** In `components/simulacra_radar/CMakeLists.txt`, add `"learn_db.c"` to `SRCS` (mbedtls is already in REQUIRES).

- [ ] **Step 6: Build + flash + read the decoy selftest** — expect `SELFTEST: PASS` with the `db key:` checks.

- [ ] **Step 7: Build-check the CYD** — `idf.py build` in `cyd/`, expect `Project build complete`.

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/learn_db.h components/simulacra_radar/learn_db.c components/simulacra_radar/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(learndb): HKDF-SHA256 at-rest key derivation (distinct from session PSK)"
```

---

### Task 3: Encrypt/decrypt the library blob (AES-256-GCM)

Seal the whole library into an encrypted, self-describing blob and open it back — host-testable round-trip + tamper rejection.

**Files:**
- Modify: `components/simulacra_radar/learn_db.h`
- Modify: `components/simulacra_radar/learn_db.c`
- Modify: `main/churn_selftest.c` (`test_learn_db_blob()`)

**Interfaces:**
- Consumes: `learn_db_derive_key` (Task 2), `learned_template_t` / `learn_regate` semantics.
- Produces:
  - On-disk header (packed): `learn_db_hdr_t { uint32 magic; uint16 version; uint16 count; uint8 nonce[12]; uint8 tag[16]; }`.
  - `int learn_db_seal(uint8_t *out, size_t *out_len, const learned_template_t *recs, uint16_t count, const uint8_t key[32]);` — writes header + AES-256-GCM ciphertext of the records array; random nonce; returns 0 on success, <0 on error or if `count` exceeds `LEARN_DB_MAX`.
  - `int learn_db_open(const uint8_t *buf, size_t len, learned_template_t *recs, uint16_t *count, const uint8_t key[32]);` — verifies magic/version, decrypts-and-authenticates; returns 0 on success, <0 on bad magic/version/length or **tag failure** (caller treats <0 as "no library").
  - `#define LEARN_DB_MAX 1024` (Vigil archive ceiling; blob = hdr + 1024*sizeof(record) ≈ 56 KB).

- [ ] **Step 1: Write the failing test** (`test_learn_db_blob();` after `test_learn_db_key();`):

```c
static void test_learn_db_blob(void)
{
    uint8_t key[32]; learn_db_derive_key(SIMULACRA_ESPNOW_KEY, key);
    learned_template_t in[3];
    for (int i = 0; i < 3; i++) { mk_shape(&in[i], (uint16_t)(0x0075 + i)); in[i].reinforce_count = (uint16_t)(i+1); }

    static uint8_t blob[sizeof(learn_db_hdr_t) + 3*sizeof(learned_template_t)]; size_t blen;
    ST_CHECK(learn_db_seal(blob, &blen, in, 3, key) == 0, "db blob: seal ok");
    ST_CHECK(blen == sizeof(learn_db_hdr_t) + 3*sizeof(learned_template_t), "db blob: length exact");

    learned_template_t out[3]; uint16_t n = 0;
    ST_CHECK(learn_db_open(blob, blen, out, &n, key) == 0 && n == 3, "db blob: open round-trips count");
    ST_CHECK(memcmp(in, out, 3*sizeof(learned_template_t)) == 0, "db blob: records identical");

    // Tamper one ciphertext byte -> tag must fail.
    static uint8_t bad[sizeof(blob)]; memcpy(bad, blob, blen);
    bad[sizeof(learn_db_hdr_t) + 4] ^= 0xFF;
    ST_CHECK(learn_db_open(bad, blen, out, &n, key) < 0, "db blob: tamper rejected");

    // Wrong key (foreign card) -> tag must fail.
    uint8_t wrong[32]; memcpy(wrong, key, 32); wrong[0] ^= 0xFF;
    ST_CHECK(learn_db_open(blob, blen, out, &n, wrong) < 0, "db blob: foreign key rejected");

    // Bad magic -> rejected.
    static uint8_t nomagic[sizeof(blob)]; memcpy(nomagic, blob, blen); nomagic[0] ^= 0xFF;
    ST_CHECK(learn_db_open(nomagic, blen, out, &n, key) < 0, "db blob: bad magic rejected");
}
```

- [ ] **Step 2: Build to verify it fails** — `undefined reference to 'learn_db_seal'`.

- [ ] **Step 3: Extend `learn_db.h`** (after the key declaration):

```c
#define LEARN_DB_MAGIC   0x4C444231u   // "LDB1"
#define LEARN_DB_FMT_VER 1
#define LEARN_DB_MAX     1024          // Vigil archive ceiling

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint8_t  nonce[12];
    uint8_t  tag[16];
} learn_db_hdr_t;

// Seal records -> [hdr | AES-256-GCM ct]. Random nonce; returns 0, <0 on error/over-cap.
int learn_db_seal(uint8_t *out, size_t *out_len, const learned_template_t *recs,
                  uint16_t count, const uint8_t key[32]);
// Open + authenticate. Returns 0 (recs/count filled), <0 on bad hdr / tag failure.
int learn_db_open(const uint8_t *buf, size_t len, learned_template_t *recs,
                  uint16_t *count, const uint8_t key[32]);
```

- [ ] **Step 4: Implement in `learn_db.c`** (mirror `radar_wire.c`'s mbedtls idiom; use the header as GCM AAD so magic/version/count are authenticated):

```c
#include "mbedtls/gcm.h"
#include "esp_random.h"    // available on both targets via esp_hw_support

int learn_db_seal(uint8_t *out, size_t *out_len, const learned_template_t *recs,
                  uint16_t count, const uint8_t key[32])
{
    if (count > LEARN_DB_MAX) return -1;
    learn_db_hdr_t *h = (learn_db_hdr_t *)out;
    h->magic = LEARN_DB_MAGIC; h->version = LEARN_DB_FMT_VER; h->count = count;
    esp_fill_random(h->nonce, sizeof h->nonce);
    size_t body = (size_t)count * sizeof(learned_template_t);
    uint8_t *ct = out + sizeof(learn_db_hdr_t);

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    // AAD = header up to (not including) the tag, so magic/version/count/nonce are authenticated.
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, body,
                        h->nonce, sizeof h->nonce,
                        (const uint8_t *)h, offsetof(learn_db_hdr_t, tag),
                        (const uint8_t *)recs, ct, sizeof h->tag, h->tag);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;
    *out_len = sizeof(learn_db_hdr_t) + body;
    return 0;
}

int learn_db_open(const uint8_t *buf, size_t len, learned_template_t *recs,
                  uint16_t *count, const uint8_t key[32])
{
    if (len < sizeof(learn_db_hdr_t)) return -1;
    const learn_db_hdr_t *h = (const learn_db_hdr_t *)buf;
    if (h->magic != LEARN_DB_MAGIC || h->version != LEARN_DB_FMT_VER) return -1;
    if (h->count > LEARN_DB_MAX) return -1;
    size_t body = (size_t)h->count * sizeof(learned_template_t);
    if (len != sizeof(learn_db_hdr_t) + body) return -1;
    const uint8_t *ct = buf + sizeof(learn_db_hdr_t);

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, body, h->nonce, sizeof h->nonce,
                        (const uint8_t *)h, offsetof(learn_db_hdr_t, tag),
                        h->tag, sizeof h->tag, ct, (uint8_t *)recs);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;                    // tag failure => corrupt/tampered/foreign
    *count = h->count;
    return 0;
}
```

Add `#include <stddef.h>` for `offsetof` (already via learn_db.h chain, but explicit is fine).

- [ ] **Step 5: Build + flash + read the decoy selftest** — expect `SELFTEST: PASS` with the five `db blob:` checks.

- [ ] **Step 6: Build-check the CYD** — expect `Project build complete`.

- [ ] **Step 7: Commit**

```bash
git add components/simulacra_radar/learn_db.h components/simulacra_radar/learn_db.c main/churn_selftest.c
git commit -m "feat(learndb): AES-256-GCM library blob seal/open (header-authenticated, tamper-rejecting)"
```

---

### Task 4: Vigil — load at boot, save atomically after merges

Wire the blob crypto to the SD file: decrypt `learn.db` into `s_lib` at boot; rewrite it atomically (`.tmp` + rename) when the library changes, debounced.

**Files:**
- Modify: `cyd/main/cyd_main.c`

**Interfaces:**
- Consumes: `sd_mount`/`s_sd_ok`/`s_card` (Task 1), `learn_db_seal`/`open`/`derive_key` (Tasks 2–3), the RAM library `s_lib`/`s_lib_count` (Phase 2b-i).
- Produces: `learn.db` persistence; `static bool s_lib_dirty;` set on every accepted merge.

- [ ] **Step 1: Add includes + paths + key cache.** In `cyd/main/cyd_main.c` add `#include "learn_db.h"` and, after the SD defines:

```c
#define LEARN_DB_PATH SD_MOUNT_POINT "/simulacra/learn.db"
#define LEARN_DB_TMP  SD_MOUNT_POINT "/simulacra/learn.tmp"
#define LEARN_DB_SAVE_MS 30000
static uint8_t  s_db_key[32];
static bool     s_lib_dirty;
```

- [ ] **Step 2: Load at boot.** Add a loader (after `sd_mount`):

```c
static void learn_db_load(void)
{
    learn_db_derive_key(SIMULACRA_ESPNOW_KEY, s_db_key);
    if (!s_sd_ok) return;
    FILE *f = fopen(LEARN_DB_PATH, "rb");
    if (!f) { ESP_LOGW(TAG, "learndb: none on card (fresh)"); return; }
    static uint8_t blob[sizeof(learn_db_hdr_t) + LEARN_DB_MAX*sizeof(learned_template_t)];
    size_t n = fread(blob, 1, sizeof blob, f); fclose(f);
    uint16_t cnt = 0;
    static learned_template_t tmp[LEARN_DB_MAX];
    if (learn_db_open(blob, n, tmp, &cnt, s_db_key) != 0) {
        ESP_LOGW(TAG, "learndb: open failed (corrupt/foreign) -> rebuild from sync");
        return;
    }
    // Re-gate every record off the card, then merge into the RAM working set (cap VIGIL_LIB_CAP).
    size_t admitted = 0;
    for (uint16_t i = 0; i < cnt; i++)
        if (learn_regate(&tmp[i]) && learn_merge_wire(s_lib, &s_lib_count, VIGIL_LIB_CAP, &tmp[i], s_lib_sweep))
            admitted++;
    ESP_LOGW(TAG, "learndb: loaded %u/%u recs -> lib=%u", (unsigned)admitted, (unsigned)cnt, (unsigned)s_lib_count);
}
```

> Note: the RAM working set is capped at `VIGIL_LIB_CAP` (128); the on-card archive may hold up to `LEARN_DB_MAX`. This task persists the working set. A larger-than-RAM archive (true long tail) is deferred — see Self-Review.

- [ ] **Step 3: Atomic save.** Add a saver:

```c
static void learn_db_save(void)
{
    if (!s_sd_ok || s_lib_count == 0) return;
    static uint8_t blob[sizeof(learn_db_hdr_t) + VIGIL_LIB_CAP*sizeof(learned_template_t)]; size_t blen;
    if (learn_db_seal(blob, &blen, s_lib, (uint16_t)s_lib_count, s_db_key) != 0) return;
    FILE *f = fopen(LEARN_DB_TMP, "wb");
    if (!f) { ESP_LOGW(TAG, "learndb: tmp open fail (keep RAM set)"); return; }
    size_t w = fwrite(blob, 1, blen, f); fclose(f);
    if (w != blen) { ESP_LOGW(TAG, "learndb: short write, abort rename"); remove(LEARN_DB_TMP); return; }
    remove(LEARN_DB_PATH);                       // FAT rename won't clobber; remove then rename
    if (rename(LEARN_DB_TMP, LEARN_DB_PATH) != 0) { ESP_LOGW(TAG, "learndb: rename fail"); return; }
    ESP_LOGW(TAG, "learndb: saved %u recs (%u B)", (unsigned)s_lib_count, (unsigned)blen);
}
```

- [ ] **Step 4: Mark dirty on merge.** In `on_recv`'s `LEARN_OFFER` block (Phase 2b-i), set the dirty flag when a merge is admitted:

```c
        for (uint8_t i = 0; i < nr; i++)
            if (learn_regate(&rx[i]))
                if (learn_merge_wire(s_lib, &s_lib_count, VIGIL_LIB_CAP, &rx[i], s_lib_sweep))
                    s_lib_dirty = true;
```

- [ ] **Step 5: Call load at boot + debounced save in the loop.** In `app_main`, after the `s_sd_ok = sd_mount();` probe block, add `learn_db_load();`. Then in the main `for(;;)` loop, alongside the sync timer:

```c
        static uint32_t last_save = 0;
        if (s_lib_dirty && now - last_save > LEARN_DB_SAVE_MS) {
            last_save = now; s_lib_dirty = false; learn_db_save();
        }
```

- [ ] **Step 6: Build the CYD** — `idf.py build`, expect `Project build complete`.

- [ ] **Step 7: Commit**

```bash
git add cyd/main/cyd_main.c
git commit -m "feat(vigil): persist RAM library to encrypted /sdcard learn.db (atomic, re-gated on load)"
```

---

### Task 5: Top-N-by-reinforce broadcast selection

Broadcast only the strongest `LEARN_SYNC_TOP_N` shapes so a full-down always fits the smallest decoy store.

**Files:**
- Modify: `components/simulacra_radar/learn_wire.h`
- Modify: `components/simulacra_radar/learn_wire.c`
- Modify: `main/churn_selftest.c` (`test_learn_top_n()`)
- Modify: `cyd/main/cyd_main.c`

**Interfaces:**
- Produces: `size_t learn_top_n(const learned_template_t *store, size_t count, learned_template_t *out, size_t n);` — copies the `n` highest-`reinforce_count` records (ties broken by newer `last_seen_sweep`) into `out`; returns `min(count, n)`. Does not mutate `store`.

- [ ] **Step 1: Write the failing test** (`test_learn_top_n();` after `test_learn_db_blob();`):

```c
static void test_learn_top_n(void)
{
    learned_template_t s[5];
    for (int i = 0; i < 5; i++) { mk_shape(&s[i], (uint16_t)(0x0070+i)); s[i].reinforce_count = (uint16_t)(i); }
    // reinforce_count = 0,1,2,3,4 ; top-3 should be the 4,3,2 set.
    learned_template_t out[3];
    size_t n = learn_top_n(s, 5, out, 3);
    ST_CHECK(n == 3, "top_n: returns n when count>=n");
    ST_CHECK(out[0].reinforce_count == 4 && out[1].reinforce_count == 3 && out[2].reinforce_count == 2,
             "top_n: strongest first");
    // n larger than count -> returns count.
    ST_CHECK(learn_top_n(s, 5, out, 3) == 3 && learn_top_n(s, 2, out, 3) == 2, "top_n: clamps to count");
}
```

- [ ] **Step 2: Build to verify it fails** — `undefined reference to 'learn_top_n'`.

- [ ] **Step 3: Declare in `learn_wire.h`** (after `learn_merge_wire`):

```c
#ifndef LEARN_SYNC_TOP_N
#define LEARN_SYNC_TOP_N 64          // smallest decoy store (Shade/C6) — a full-down fits every decoy
#endif

// Copy the n strongest records (by reinforce_count, ties: newer last_seen_sweep) into out.
// Does not mutate store. Returns min(count, n).
size_t learn_top_n(const learned_template_t *store, size_t count, learned_template_t *out, size_t n);
```

- [ ] **Step 4: Implement in `learn_wire.c`** (partial selection — `count` is small, so a simple selection scan is fine and avoids mutating `store`):

```c
size_t learn_top_n(const learned_template_t *store, size_t count, learned_template_t *out, size_t n)
{
    size_t take = (count < n) ? count : n;
    bool used[/* cap */ 256] = { false };          // count <= LEARN_DB_MAX working set; guard below
    for (size_t k = 0; k < take; k++) {
        int best = -1;
        for (size_t i = 0; i < count && i < 256; i++) {
            if (used[i]) continue;
            if (best < 0 ||
                store[i].reinforce_count > store[best].reinforce_count ||
                (store[i].reinforce_count == store[best].reinforce_count &&
                 store[i].last_seen_sweep > store[best].last_seen_sweep)) best = (int)i;
        }
        if (best < 0) break;
        used[best] = true; out[k] = store[(size_t)best];
    }
    return take;
}
```

> `used[256]` bounds the selection to the first 256 records — the Vigil RAM working set (`VIGIL_LIB_CAP` 128) is well within that. When Task-4-note's larger archive lands, replace this with a heap.

- [ ] **Step 5: Build + flash + read the decoy selftest** — expect `SELFTEST: PASS` with `top_n:` checks.

- [ ] **Step 6: Use it in Vigil's broadcast.** In `cyd/main/cyd_main.c` `broadcast_library()`, select the top-N into a scratch buffer and send that instead of the whole `s_lib`:

```c
static void broadcast_library(void){
    if (s_lib_count == 0) return;
    s_lib_sweep++;
    static learned_template_t sel[LEARN_SYNC_TOP_N];
    size_t n = learn_top_n(s_lib, s_lib_count, sel, LEARN_SYNC_TOP_N);
    uint8_t chunks = (uint8_t)((n + LEARN_WIRE_RECS_PER_CHUNK - 1) / LEARN_WIRE_RECS_PER_CHUNK);
    for (uint8_t ci = 0; ci < chunks; ci++) {
        size_t off = (size_t)ci * LEARN_WIRE_RECS_PER_CHUNK;
        uint8_t nrec = (uint8_t)((n - off < LEARN_WIRE_RECS_PER_CHUNK) ? (n - off) : LEARN_WIRE_RECS_PER_CHUNK);
        uint8_t pl[RADAR_FRAME_MAX]; size_t plen;
        if (learn_wire_pack(pl, &plen, &sel[off], nrec, 1, ci, chunks) != 0) continue;
        uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
        if (radar_wire_seal(frame, &flen, RADAR_TYPE_LEARN_SYNC, pl, plen,
                            SIMULACRA_ESPNOW_KEY, s_salt, ++s_ctr) == 0)
            esp_now_send(BCAST, frame, flen);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "broadcast top-%u of %u recs", (unsigned)n, (unsigned)s_lib_count);
}
```

- [ ] **Step 7: Build the CYD** — expect `Project build complete`.

- [ ] **Step 8: Commit**

```bash
git add components/simulacra_radar/learn_wire.h components/simulacra_radar/learn_wire.c main/churn_selftest.c cyd/main/cyd_main.c
git commit -m "feat(learn): top-N-by-reinforce broadcast selection (fits smallest decoy cap)"
```

---

### Task 6: On-target persistence + graceful-degradation verification

Prove the library survives a reboot, a pulled card degrades gracefully, and a corrupt file rebuilds.

**Files:** none (verification only).

- [ ] **Step 1: Seed + persist.** With a microSD inserted in the CYD, flash Vigil (COM10) and the NVS-seeded decoy (COM12, normal Ward build, which offers 2 shapes as in Phase 2b-i). Read Vigil ~90 s:

```powershell
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM10 --seconds 90 --reset yes --grep "sd:|learndb|offer rx|broadcast"
```
Expected: `sd: mounted`, `learndb: none on card (fresh)` (first boot), `offer rx: +2 lib=2`, then within 30 s `learndb: saved 2 recs`.

- [ ] **Step 2: Reboot persists.** Reset Vigil only (decoy off or out of range) and read again:

```powershell
python "$env:USERPROFILE\.claude\skills\build-flash-read\read_serial.py" --port COM10 --seconds 20 --reset yes --grep "sd:|learndb"
```
Expected: `sd: mounted`, `learndb: loaded 2/2 recs -> lib=2` — **the shapes survived the reboot with no decoy present.**

- [ ] **Step 3: Pull-card graceful.** Eject the microSD, reset Vigil, read:
Expected: `sd: absent/unmountable ... RAM-only librarian`, radar still renders, `broadcast top-…` still fires (RAM path), no crash.

- [ ] **Step 4: Corrupt rebuilds.** (Optional, if a card reader is handy) Flip a byte in `learn.db` on a host, reinsert, reset Vigil:
Expected: `learndb: open failed (corrupt/foreign) -> rebuild from sync`, then normal operation; next decoy offer re-populates and re-saves.

- [ ] **Step 5: Restore boards.** Leave Vigil on the new firmware; reflash the decoy to the display-paired Ward build (`-DSIMULACRA_ESPNOW=1 -DSIMULACRA_WEBUI=1`).

- [ ] **Step 6 (optional): Commit any bench tweak** (e.g., SPI2 fallback from Task 1, or a pin correction). Otherwise nothing to commit.

---

## Self-Review

**Spec coverage (`2026-07-03-fleet-template-sync-design.md` §SD storage / §Testing / §Configuration):**
- Mount via `esp_vfs_fat_sdspi_mount` (§SD Mount) → Task 1.
- `/sdcard/simulacra/learn.db`, header + records, atomic `.tmp`+rename (§SD File) → Tasks 3–4.
- AES-256-GCM at rest, HKDF key under dedicated label, tag-fail→"no library" (§At-rest encryption) → Tasks 2–4.
- Graceful SD-absent (§Graceful, §Error handling) → Tasks 1, 4, 6.
- Top-N-by-reinforce fits smallest decoy cap (§SD superset, §Testing) → Task 5.
- Hardware caveat validated early (§Hardware caveat) → Task 1 spike (+ SPI2 fallback path).
- Config symbols `SD_MOUNT_POINT`/`LEARN_DB_PATH`/`LEARN_SYNC_TOP_N` (§Configuration) → Tasks 1/4/5.

**Deferred (out of scope for this plan, noted for a Phase 2b-iii):**
- **Vigil UI page** (§Vigil UI) — spec says "low priority"; no touch-page work here.
- **Archive larger than the RAM working set** — this plan persists the `VIGIL_LIB_CAP` (128) working set; the "thousands of shapes / long tail on SD" superset (§SD superset) needs a streaming load/merge and a heap-based top-N (flagged in Tasks 4–5 notes).
- **3-board soak with a real Shade C6** (§Testing on-target) — Task 6 proves persistence + graceful paths with the 2 boards on the bench; the Ward→Shade cross-propagation soak is a follow-on when the C6 is provisioned with the same PSK.
- **Dirty-bit** (§Testing) — intentionally dropped (E1); full-library idempotent sync from Phase 2b-i supersedes it.

**Placeholder scan:** every code step is complete. Task 1 Step 4 and Task 6 Steps 3–4 flag genuine bench decisions (card presence, bus fallback), not vague code.

**Type consistency:** `learn_db_derive_key(psk,out)`, `learn_db_seal(out,&len,recs,count,key)`, `learn_db_open(buf,len,recs,&count,key)`, `learn_top_n(store,count,out,n)` are consistent across their Task definitions and all call sites (Tasks 4–5, cyd_main). `learn_db_hdr_t`/`LEARN_DB_MAGIC`/`LEARN_DB_MAX`/`LEARN_SYNC_TOP_N`/`VIGIL_LIB_CAP` used consistently. `s_sd_ok`/`s_card`/`s_db_key`/`s_lib_dirty` are the Vigil-side statics. Reuses `learn_regate`/`learn_merge_wire`/`s_lib`/`s_lib_count`/`s_lib_sweep` from Phase 2a/2b-i with matching signatures.
```
