#include <string.h>
#include <stddef.h>
#include "learn_db.h"
#include "mbedtls/md.h"
#include "mbedtls/gcm.h"
#include "esp_random.h"

// HKDF-SHA256 (RFC 5869), implemented on HMAC-SHA256 so we don't depend on
// MBEDTLS_HKDF_C being enabled (it isn't in the default ESP-IDF mbedtls config).
// Single 32-byte output => Expand is one block.
void learn_db_derive_key(const uint8_t psk[32], uint8_t out_key[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    // Extract: PRK = HMAC(salt = 32 zero bytes, IKM = psk)
    uint8_t zero_salt[32] = { 0 };
    uint8_t prk[32];
    mbedtls_md_hmac(md, zero_salt, sizeof zero_salt, psk, 32, prk);

    // Expand (L = 32 = HashLen): T(1) = HMAC(PRK, info || 0x01); OKM = T(1)
    uint8_t info_blk[64];
    size_t  info_len = strlen(LEARN_DB_LABEL);
    memcpy(info_blk, LEARN_DB_LABEL, info_len);
    info_blk[info_len] = 0x01;
    mbedtls_md_hmac(md, prk, sizeof prk, info_blk, info_len + 1, out_key);
}

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
