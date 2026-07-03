#include "radar_wire.h"
#include <string.h>
#include "mbedtls/gcm.h"

static void make_nonce(uint8_t nonce[12], const uint8_t salt[4], uint64_t counter)
{
    memcpy(nonce, salt, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(counter >> (8 * (7 - i)));  // big-endian
}

int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[32], const uint8_t salt[4], uint64_t counter)
{
    if (RADAR_HDR_LEN + RADAR_NONCE_LEN + payload_len + RADAR_TAG_LEN > RADAR_FRAME_MAX) return -1;
    frame[0] = RADAR_MAGIC0; frame[1] = RADAR_MAGIC1; frame[2] = RADAR_WIRE_VER; frame[3] = type;
    uint8_t nonce[12]; make_nonce(nonce, salt, counter);
    memcpy(frame + RADAR_HDR_LEN, nonce, RADAR_NONCE_LEN);
    uint8_t *ct  = frame + RADAR_HDR_LEN + RADAR_NONCE_LEN;
    uint8_t *tag = ct + payload_len;

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, payload_len,
                          nonce, RADAR_NONCE_LEN, frame, RADAR_HDR_LEN,   // AAD = header
                          payload, ct, RADAR_TAG_LEN, tag);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;
    *frame_len = RADAR_HDR_LEN + RADAR_NONCE_LEN + payload_len + RADAR_TAG_LEN;
    return 0;
}

int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32],
                    uint8_t *out_type, uint8_t *payload, size_t *payload_len,
                    uint8_t out_salt[4], uint64_t *out_counter)
{
    if (frame_len < RADAR_HDR_LEN + RADAR_NONCE_LEN + RADAR_TAG_LEN) return -1;
    if (frame[0] != RADAR_MAGIC0 || frame[1] != RADAR_MAGIC1 || frame[2] != RADAR_WIRE_VER) return -1;
    const uint8_t *nonce = frame + RADAR_HDR_LEN;
    size_t pl = frame_len - RADAR_HDR_LEN - RADAR_NONCE_LEN - RADAR_TAG_LEN;
    const uint8_t *ct  = nonce + RADAR_NONCE_LEN;
    const uint8_t *tag = ct + pl;

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, pl, nonce, RADAR_NONCE_LEN,
                          frame, RADAR_HDR_LEN, tag, RADAR_TAG_LEN, ct, payload);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;                             // bad tag / bad key
    *out_type = frame[3]; *payload_len = pl;
    memcpy(out_salt, nonce, 4);
    uint64_t c = 0; for (int i = 0; i < 8; i++) c = (c << 8) | nonce[4 + i];
    *out_counter = c;
    return 0;
}

bool radar_replay_ok(radar_replay_t *st, const uint8_t salt[4], uint64_t counter)
{
    if (!st->seen || memcmp(st->salt, salt, 4) != 0) {     // fresh or peer rebooted
        memcpy(st->salt, salt, 4); st->counter = counter; st->seen = true; return true;
    }
    if (counter > st->counter) { st->counter = counter; return true; }
    return false;
}
