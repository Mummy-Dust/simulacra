#include <string.h>
#include "learn_db.h"
#include "mbedtls/md.h"

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
