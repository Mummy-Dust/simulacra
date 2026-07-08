#include "fleet_db.h"
#include "sim_ctrl_sk.h"          // SIMULACRA_CTRL_SK[64] — sealing-key material
#include "learn_db.h"             // reuse learn_db_derive_key (HKDF-SHA256)
#include "mbedtls/gcm.h"
#include "esp_random.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#define FLEET_DB_MAGIC   0x31424C46u   // "FLB1"
#define FLEET_DB_FMT_VER 1
#define FLEET_DB_PATH    "/sdcard/simulacra/fleet.db"
#define FLEET_DB_TMP     "/sdcard/simulacra/fleet.tmp"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;          // allowlist entries
    uint32_t epoch;
    uint8_t  nonce[12];
    uint8_t  tag[16];
} fleet_db_hdr_t;             // AAD = everything up to (not including) tag

static const char *TAG = "fleetdb";

static uint8_t  s_key[32];
static uint32_t s_epoch;
static uint8_t  s_allow[FLEET_ALLOW_CAP][32];
static uint16_t s_allow_n;
static bool     s_ready;

static void seal_key(uint8_t out[32])
{
    // Derive from the control secret seed (first 32 B of the 64-B Ed25519 sk). Distinct
    // from the learn.db key (which derives from SIMULACRA_ESPNOW_KEY).
    learn_db_derive_key(SIMULACRA_CTRL_SK, out);
}

size_t fleet_db_blob_cap(void)
{
    return sizeof(fleet_db_hdr_t) + 32 + FLEET_ALLOW_CAP * 32;
}

int fleet_db_seal_blob(uint8_t *out, size_t *out_len)
{
    fleet_db_hdr_t *h = (fleet_db_hdr_t *)out;
    h->magic = FLEET_DB_MAGIC; h->version = FLEET_DB_FMT_VER;
    h->count = s_allow_n; h->epoch = s_epoch;
    esp_fill_random(h->nonce, sizeof h->nonce);

    // Plaintext body = key(32) || allowlist(count*32)
    size_t body = 32 + (size_t)s_allow_n * 32;
    uint8_t pt[32 + FLEET_ALLOW_CAP * 32];
    memcpy(pt, s_key, 32);
    memcpy(pt + 32, s_allow, (size_t)s_allow_n * 32);
    uint8_t *ct = out + sizeof(fleet_db_hdr_t);

    uint8_t k[32]; seal_key(k);
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, k, 256);
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, body,
                        h->nonce, sizeof h->nonce,
                        (const uint8_t *)h, offsetof(fleet_db_hdr_t, tag),
                        pt, ct, sizeof h->tag, h->tag);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;
    *out_len = sizeof(fleet_db_hdr_t) + body;
    return 0;
}

int fleet_db_open_blob(const uint8_t *buf, size_t len)
{
    if (len < sizeof(fleet_db_hdr_t)) return -1;
    const fleet_db_hdr_t *h = (const fleet_db_hdr_t *)buf;
    if (h->magic != FLEET_DB_MAGIC || h->version != FLEET_DB_FMT_VER) return -1;
    if (h->count > FLEET_ALLOW_CAP) return -1;
    size_t body = 32 + (size_t)h->count * 32;
    if (len != sizeof(fleet_db_hdr_t) + body) return -1;
    const uint8_t *ct = buf + sizeof(fleet_db_hdr_t);

    uint8_t pt[32 + FLEET_ALLOW_CAP * 32];
    uint8_t k[32]; seal_key(k);
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, k, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, body, h->nonce, sizeof h->nonce,
                        (const uint8_t *)h, offsetof(fleet_db_hdr_t, tag),
                        h->tag, sizeof h->tag, ct, pt);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;                 // tag failure => corrupt/tampered/foreign card

    memcpy(s_key, pt, 32);
    s_epoch = h->epoch;
    s_allow_n = h->count;
    memcpy(s_allow, pt + 32, (size_t)s_allow_n * 32);
    s_ready = true;
    return 0;
}

void fleet_db_load(void)
{
    FILE *f = fopen(FLEET_DB_PATH, "rb");
    if (f) {
        static uint8_t blob[sizeof(fleet_db_hdr_t) + 32 + FLEET_ALLOW_CAP * 32];
        size_t n = fread(blob, 1, sizeof blob, f);
        fclose(f);
        if (fleet_db_open_blob(blob, n) == 0) {
            ESP_LOGW(TAG, "loaded: epoch %u, %u allowed", (unsigned)s_epoch, (unsigned)s_allow_n);
            return;
        }
        ESP_LOGW(TAG, "fleet.db unreadable/foreign -- reinitializing");
    }
    // First run (or unreadable): fresh random key, empty allowlist, epoch 1.
    esp_fill_random(s_key, sizeof s_key);
    s_epoch = 1; s_allow_n = 0; s_ready = true;
    ESP_LOGW(TAG, "initialized new fleet key (epoch 1)");
}

void fleet_db_save(void)
{
    if (!s_ready) return;
    static uint8_t blob[sizeof(fleet_db_hdr_t) + 32 + FLEET_ALLOW_CAP * 32]; size_t blen;
    if (fleet_db_seal_blob(blob, &blen) != 0) return;
    FILE *f = fopen(FLEET_DB_TMP, "wb");
    if (!f) { ESP_LOGW(TAG, "save: cannot open tmp"); return; }
    size_t w = fwrite(blob, 1, blen, f);
    fclose(f);
    if (w != blen) { ESP_LOGW(TAG, "save: short write"); return; }
    remove(FLEET_DB_PATH);
    if (rename(FLEET_DB_TMP, FLEET_DB_PATH) != 0) { ESP_LOGW(TAG, "save: rename failed"); return; }
    ESP_LOGW(TAG, "saved: epoch %u, %u allowed (%u B)", (unsigned)s_epoch, (unsigned)s_allow_n, (unsigned)blen);
}

const uint8_t *fleet_db_key(void)  { return s_key; }
uint32_t       fleet_db_epoch(void) { return s_epoch; }

void fleet_db_rotate(void)
{
    esp_fill_random(s_key, sizeof s_key);
    s_epoch++;
}

static int allow_index(const uint8_t id_pk[32])
{
    for (uint16_t i = 0; i < s_allow_n; i++)
        if (memcmp(s_allow[i], id_pk, 32) == 0) return (int)i;
    return -1;
}

bool fleet_allow_contains(const uint8_t id_pk[32]) { return allow_index(id_pk) >= 0; }

bool fleet_allow_add(const uint8_t id_pk[32])
{
    if (allow_index(id_pk) >= 0) return true;       // already present
    if (s_allow_n >= FLEET_ALLOW_CAP) return false; // full
    memcpy(s_allow[s_allow_n++], id_pk, 32);
    return true;
}

bool fleet_allow_remove(const uint8_t id_pk[32])
{
    int i = allow_index(id_pk);
    if (i < 0) return false;
    s_allow_n--;
    if ((uint16_t)i != s_allow_n) memcpy(s_allow[i], s_allow[s_allow_n], 32);  // swap-remove
    return true;
}

size_t fleet_allow_count(void) { return s_allow_n; }
const uint8_t *fleet_allow_at(size_t i) { return (i < s_allow_n) ? s_allow[i] : NULL; }

int fleet_db_selftest(void)
{
    int fail = 0;
    // Fresh state
    esp_fill_random(s_key, sizeof s_key); s_epoch = 3; s_allow_n = 0; s_ready = true;
    uint8_t a[32], b[32];
    for (int i = 0; i < 32; i++) { a[i] = (uint8_t)(i + 1); b[i] = (uint8_t)(0xA0 + i); }

    if (!fleet_allow_add(a) || !fleet_allow_contains(a)) fail++;
    if (fleet_allow_contains(b)) fail++;                       // not added yet
    if (!fleet_allow_add(b) || fleet_allow_count() != 2) fail++;
    if (!fleet_allow_add(a) || fleet_allow_count() != 2) fail++; // dup no-op

    uint8_t key_snap[32]; memcpy(key_snap, s_key, 32);
    static uint8_t blob[sizeof(fleet_db_hdr_t) + 32 + FLEET_ALLOW_CAP * 32]; size_t blen;
    if (fleet_db_seal_blob(blob, &blen) != 0) fail++;
    // Corrupt copy to prove tamper detection
    uint8_t saved = blob[blen - 1]; blob[blen - 1] ^= 0x01;
    if (fleet_db_open_blob(blob, blen) == 0) fail++;           // tampered must fail
    blob[blen - 1] = saved;
    s_allow_n = 0; s_epoch = 0; memset(s_key, 0, 32);          // wipe, then restore from blob
    if (fleet_db_open_blob(blob, blen) != 0) fail++;
    if (s_epoch != 3 || s_allow_n != 2 || memcmp(s_key, key_snap, 32) != 0) fail++;
    if (!fleet_allow_contains(a) || !fleet_allow_contains(b)) fail++;

    if (!fleet_allow_remove(a) || fleet_allow_contains(a) || fleet_allow_count() != 1) fail++;
    if (fleet_allow_remove(a)) fail++;                        // already gone

    uint32_t e0 = s_epoch; fleet_db_rotate();
    if (s_epoch != e0 + 1 || memcmp(s_key, key_snap, 32) == 0) fail++;  // new key + epoch bump

    ESP_LOGW(TAG, "fleet_db selftest: %s (%d fails)", fail ? "FAIL" : "PASS", fail);
    return fail;
}
