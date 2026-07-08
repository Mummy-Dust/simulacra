#pragma once
#include <stdint.h>
#include <stddef.h>

// Raw ESP-NOW enrollment frame types (NOT radar_wire_seal-wrapped: an un-enrolled
// decoy has no fleet key, so these frames carry their own ECDH/Ed25519 security).
#define RADAR_TYPE_ENROLL_OFFER   8
#define RADAR_TYPE_ENROLL_REQUEST 9
#define RADAR_TYPE_ENROLL_GRANT   10

#define ENROLL_VER       1
#define ENROLL_PK_LEN    32
#define ENROLL_NONCE_LEN 24
#define ENROLL_SIG_LEN   64
#define FLEET_KEY_LEN    32

// OFFER  = ver(1) | vigil_eph_pk(32) | nonce_v(24) | epoch(4 LE) | sig(64)
#define ENROLL_OFFER_LEN   (1 + 32 + 24 + 4 + 64)      // 125
// REQUEST= ver(1) | id_pk(32) | nonce_d(24) | box_nonce(24) | box(16 + [tag(4)|nonce_v(24)]=28) = 44
#define ENROLL_REQUEST_LEN (1 + 32 + 24 + 24 + 44)     // 125
// GRANT  = ver(1) | box_nonce(24) | box(16 + [tag(4)|k_fleet(32)|epoch(4)|nonce_d(24)]=64) = 80
#define ENROLL_GRANT_LEN   (1 + 24 + 80)               // 105

// --- OFFER: Vigil -> broadcast, Ed25519-signed. Decoy authenticates Vigil via ctrl_pk. ---
int enroll_offer_sign(uint8_t *out, size_t cap, const uint8_t vigil_eph_pk[32],
                      const uint8_t nonce_v[24], uint32_t epoch, const uint8_t ctrl_sk[64]);
int enroll_offer_open(const uint8_t *in, size_t len, const uint8_t ctrl_pk[32],
                      uint8_t vigil_eph_pk_out[32], uint8_t nonce_v_out[24], uint32_t *epoch_out);

// --- REQUEST: decoy -> Vigil. box proves possession of id_sk and echoes nonce_v. ---
int enroll_request_build(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t id_sk[32],
                         const uint8_t nonce_d[24], const uint8_t vigil_eph_pk[32], const uint8_t nonce_v[24]);
int enroll_request_open(const uint8_t *in, size_t len, const uint8_t vigil_eph_sk[32],
                        uint8_t id_pk_out[32], uint8_t nonce_d_out[24], uint8_t nonce_v_echo_out[24]);

// --- GRANT: Vigil -> decoy. Seals the fleet key to id_pk; echoes nonce_d for freshness. ---
int enroll_grant_seal(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t vigil_eph_sk[32],
                      const uint8_t nonce_d[24], const uint8_t k_fleet[32], uint32_t epoch);
int enroll_grant_open(const uint8_t *in, size_t len, const uint8_t vigil_eph_pk[32], const uint8_t id_sk[32],
                      uint8_t k_fleet_out[32], uint32_t *epoch_out, uint8_t nonce_d_echo_out[24]);
