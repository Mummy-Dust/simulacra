#include "enroll_wire.h"
#include "tweetnacl.h"
#include <string.h>

extern void randombytes(unsigned char *, unsigned long long);

static const uint8_t TAG_OFFER[4] = { 'E','O','F','R' };
static const uint8_t TAG_REQ[4]   = { 'E','R','E','Q' };
static const uint8_t TAG_GRANT[4] = { 'E','G','R','N' };

#define ZB  crypto_box_ZEROBYTES      // 32
#define BZB crypto_box_BOXZEROBYTES   // 16
#define BOXBUF 160                    // >= ZB + max plaintext(64)

// Encrypt ptlen bytes -> (BZB + ptlen) wire bytes (MAC||ct). Returns wire len or -1.
// Hides the NaCl zero-padding convention: the 32 ZEROBYTES / 16 BOXZEROBYTES never hit the wire.
static int box_seal(uint8_t *out, const uint8_t *pt, size_t ptlen, const uint8_t nonce[24],
                    const uint8_t to_pk[32], const uint8_t from_sk[32])
{
    if (ZB + ptlen > BOXBUF) return -1;
    uint8_t m[BOXBUF], c[BOXBUF];
    memset(m, 0, ZB); memcpy(m + ZB, pt, ptlen);
    if (crypto_box(c, m, ZB + ptlen, nonce, to_pk, from_sk) != 0) return -1;
    memcpy(out, c + BZB, (ZB + ptlen) - BZB);      // strip the BZB leading zeros
    return (int)((ZB + ptlen) - BZB);
}

// Inverse: wirelen bytes (MAC||ct) -> (wirelen - BZB) plaintext bytes into pt. Returns 0/-1.
static int box_open(uint8_t *pt, const uint8_t *wire, size_t wirelen, const uint8_t nonce[24],
                    const uint8_t from_pk[32], const uint8_t to_sk[32])
{
    size_t clen = BZB + wirelen;
    if (clen > BOXBUF) return -1;
    uint8_t c[BOXBUF], m[BOXBUF];
    memset(c, 0, BZB); memcpy(c + BZB, wire, wirelen);
    if (crypto_box_open(m, c, clen, nonce, from_pk, to_sk) != 0) return -1;
    memcpy(pt, m + ZB, clen - ZB);
    return 0;
}

// ---- OFFER (Ed25519-signed, no encryption) ----
#define OFFER_SIGNED_LEN (4 + 32 + 24 + 4)   // tag|eph_pk|nonce_v|epoch
static void offer_signed_msg(uint8_t m[OFFER_SIGNED_LEN], const uint8_t eph[32],
                             const uint8_t nv[24], uint32_t epoch)
{
    memcpy(m, TAG_OFFER, 4); memcpy(m + 4, eph, 32); memcpy(m + 36, nv, 24);
    m[60] = (uint8_t)epoch;        m[61] = (uint8_t)(epoch >> 8);
    m[62] = (uint8_t)(epoch >> 16); m[63] = (uint8_t)(epoch >> 24);
}

int enroll_offer_sign(uint8_t *out, size_t cap, const uint8_t veph[32], const uint8_t nv[24],
                      uint32_t epoch, const uint8_t ctrl_sk[64])
{
    if (cap < ENROLL_OFFER_LEN) return -1;
    uint8_t msg[OFFER_SIGNED_LEN]; offer_signed_msg(msg, veph, nv, epoch);
    uint8_t sm[64 + OFFER_SIGNED_LEN]; unsigned long long smlen = 0;
    if (crypto_sign(sm, &smlen, msg, OFFER_SIGNED_LEN, ctrl_sk) != 0) return -1;
    out[0] = ENROLL_VER; memcpy(out + 1, veph, 32); memcpy(out + 33, nv, 24);
    memcpy(out + 57, msg + 60, 4);              // epoch (4)
    memcpy(out + 61, sm, 64);                   // detached sig = first 64 bytes of sm
    return ENROLL_OFFER_LEN;
}

int enroll_offer_open(const uint8_t *in, size_t len, const uint8_t ctrl_pk[32],
                      uint8_t veph_out[32], uint8_t nv_out[24], uint32_t *epoch_out)
{
    if (len != ENROLL_OFFER_LEN || in[0] != ENROLL_VER) return -1;
    uint32_t epoch = (uint32_t)in[57] | ((uint32_t)in[58] << 8) | ((uint32_t)in[59] << 16) | ((uint32_t)in[60] << 24);
    uint8_t msg[OFFER_SIGNED_LEN]; offer_signed_msg(msg, in + 1, in + 33, epoch);
    uint8_t sm[64 + OFFER_SIGNED_LEN], rec[64 + OFFER_SIGNED_LEN]; unsigned long long rlen = 0;
    memcpy(sm, in + 61, 64); memcpy(sm + 64, msg, OFFER_SIGNED_LEN);
    if (crypto_sign_open(rec, &rlen, sm, 64 + OFFER_SIGNED_LEN, ctrl_pk) != 0) return -1;
    if (rlen != OFFER_SIGNED_LEN) return -1;
    memcpy(veph_out, in + 1, 32); memcpy(nv_out, in + 33, 24); *epoch_out = epoch;
    return 0;
}

// ---- REQUEST (box: TAG_REQ|nonce_v, from id_sk to vigil_eph_pk) ----
int enroll_request_build(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t id_sk[32],
                         const uint8_t nonce_d[24], const uint8_t veph[32], const uint8_t nv[24])
{
    if (cap < ENROLL_REQUEST_LEN) return -1;
    uint8_t bn[24]; randombytes(bn, 24);
    uint8_t pt[4 + 24]; memcpy(pt, TAG_REQ, 4); memcpy(pt + 4, nv, 24);
    out[0] = ENROLL_VER; memcpy(out + 1, id_pk, 32); memcpy(out + 33, nonce_d, 24); memcpy(out + 57, bn, 24);
    int bl = box_seal(out + 81, pt, sizeof pt, bn, veph, id_sk);
    if (bl != 44) return -1;
    return ENROLL_REQUEST_LEN;
}

int enroll_request_open(const uint8_t *in, size_t len, const uint8_t veph_sk[32],
                        uint8_t id_pk_out[32], uint8_t nonce_d_out[24], uint8_t nv_echo_out[24])
{
    if (len != ENROLL_REQUEST_LEN || in[0] != ENROLL_VER) return -1;
    const uint8_t *id_pk = in + 1, *bn = in + 57, *box = in + 81;
    uint8_t pt[4 + 24];
    if (box_open(pt, box, 44, bn, id_pk, veph_sk) != 0) return -1;
    if (memcmp(pt, TAG_REQ, 4) != 0) return -1;
    memcpy(id_pk_out, id_pk, 32); memcpy(nonce_d_out, in + 33, 24); memcpy(nv_echo_out, pt + 4, 24);
    return 0;
}

// ---- GRANT (box: TAG_GRANT|k_fleet|epoch|nonce_d, from vigil_eph_sk to id_pk) ----
int enroll_grant_seal(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t veph_sk[32],
                      const uint8_t nonce_d[24], const uint8_t k_fleet[32], uint32_t epoch)
{
    if (cap < ENROLL_GRANT_LEN) return -1;
    uint8_t bn[24]; randombytes(bn, 24);
    uint8_t pt[4 + 32 + 4 + 24];
    memcpy(pt, TAG_GRANT, 4); memcpy(pt + 4, k_fleet, 32);
    pt[36] = (uint8_t)epoch;        pt[37] = (uint8_t)(epoch >> 8);
    pt[38] = (uint8_t)(epoch >> 16); pt[39] = (uint8_t)(epoch >> 24);
    memcpy(pt + 40, nonce_d, 24);
    out[0] = ENROLL_VER; memcpy(out + 1, bn, 24);
    int bl = box_seal(out + 25, pt, sizeof pt, bn, id_pk, veph_sk);
    if (bl != 80) return -1;
    return ENROLL_GRANT_LEN;
}

int enroll_grant_open(const uint8_t *in, size_t len, const uint8_t veph[32], const uint8_t id_sk[32],
                      uint8_t k_fleet_out[32], uint32_t *epoch_out, uint8_t nonce_d_echo_out[24])
{
    if (len != ENROLL_GRANT_LEN || in[0] != ENROLL_VER) return -1;
    const uint8_t *bn = in + 1, *box = in + 25;
    uint8_t pt[4 + 32 + 4 + 24];
    if (box_open(pt, box, 80, bn, veph, id_sk) != 0) return -1;
    if (memcmp(pt, TAG_GRANT, 4) != 0) return -1;
    memcpy(k_fleet_out, pt + 4, 32);
    *epoch_out = (uint32_t)pt[36] | ((uint32_t)pt[37] << 8) | ((uint32_t)pt[38] << 16) | ((uint32_t)pt[39] << 24);
    memcpy(nonce_d_echo_out, pt + 40, 24);
    return 0;
}
