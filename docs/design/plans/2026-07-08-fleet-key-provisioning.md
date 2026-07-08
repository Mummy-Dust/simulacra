# Fleet Key Provisioning (ECDH Enrollment) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A decoy ships with no fleet transport key and joins the fleet via an on-air, mutually-authenticated ECDH handshake with Vigil, storing the sealed key in NVS; add rotation + per-node revocation.

**Architecture:** New pure `enroll_wire` crypto module (TweetNaCl `crypto_sign` + `crypto_box`) implements a 3-message handshake (signed OFFER → identity REQUEST → sealed GRANT). Decoys gain an NVS identity keypair + fleet-key store and a seek-enrollment responder in `esp_now_link.c`. Vigil gains an SD-sealed allowlist + fleet key, a pairing-window authority with a TOFU accept UI, and rotate/revoke actions.

**Tech Stack:** ESP-IDF v5.5 (C5) / v5.4 (C6, CYD); vendored TweetNaCl (`components/tweetnacl`); existing `learn_db` AES-256-GCM/HKDF sealing; NVS; ESP-NOW raw broadcast.

## Global Constants

Copied verbatim from the spec; every task's requirements implicitly include these.

- **Root of trust stays baked:** `SIMULACRA_CTRL_PK` (Ed25519 public) remains a compile-time header on decoys — it authenticates Vigil during enrollment. Only the symmetric fleet key moves to NVS.
- **New raw ESP-NOW frame types (NOT `radar_wire_seal`-wrapped):** `RADAR_TYPE_ENROLL_OFFER=8`, `RADAR_TYPE_ENROLL_REQUEST=9`, `RADAR_TYPE_ENROLL_GRANT=10`. Types 1–7 are taken.
- **Field sizes:** identity/ephemeral pubkey 32 B (`crypto_box` X25519); box nonce 24 B (`crypto_box_NONCEBYTES`); Ed25519 detached sig 64 B; fleet key 32 B; epoch `uint32` little-endian.
- **NaCl box wire form:** a `crypto_box` of `P` plaintext bytes is transmitted as `16 + P` bytes (MAC ‖ ciphertext); the 16 leading `BOXZEROBYTES` and 32 `ZEROBYTES` padding are handled inside `enroll_wire.c`, never on the wire.
- **Detached signatures** mirror `config_wire.c`: `crypto_sign` into a scratch buffer, keep the 64-byte prefix; verify by reassembling `sig ‖ msg` and `crypto_sign_open`.
- **Build gate:** `SIMULACRA_FLEET_PROVISION` must be added to the `foreach(flag …)` forwarder in `main/CMakeLists.txt` — that is the only path by which `-D` reaches the compiler. When the gate is **off**, `radar_key.h`'s compile-time `SIMULACRA_ESPNOW_KEY` remains the key so existing builds/tests are unaffected.
- **Ward decoy build flags:** `-DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1`.
- **Selftests run ON-TARGET** via `-DCHURN_SELFTEST=1`; `ST_CHECK(cond,msg)` increments `s_total`, logs `FAIL:` on failure; `churn_selftest_run()` prints `selftest: N/N pass`. Register each new `test_*()` by calling it in `churn_selftest_run()`.
- **Crypto stack budget:** main-task stack is already 12288 for TweetNaCl signing ([[vigil-remote-control]]); no further bump expected — confirm in Task 7 soak.
- **Domain-separation tags (4 B each):** `tag_OFFER="EOFR"`, `tag_REQ="EREQ"`, `tag_GRANT="EGRN"` (ASCII, as `uint8_t[4]`).

---

### Task 1: Enrollment wire protocol — crypto core

Pure module: pack/parse + sign/box for the three messages. Depends only on `tweetnacl`. Fully self-tested on target; no hardware, no NVS, no ESP-NOW.

**Files:**
- Create: `components/simulacra_radar/enroll_wire.h`
- Create: `components/simulacra_radar/enroll_wire.c`
- Modify: `components/simulacra_radar/CMakeLists.txt` (add `enroll_wire.c` to `SRCS`; ensure `tweetnacl` in `REQUIRES`)
- Test: `main/churn_selftest.c` (add `test_enroll_wire()` + register it)

**Interfaces:**
- Consumes: TweetNaCl `crypto_sign/_open`, `crypto_box/_open`, `crypto_box_keypair`, `randombytes`.
- Produces (used by Tasks 3 & 5):
  - `int enroll_offer_sign(uint8_t *out, size_t cap, const uint8_t vigil_eph_pk[32], const uint8_t nonce_v[24], uint32_t epoch, const uint8_t ctrl_sk[64]);` → `ENROLL_OFFER_LEN` or −1.
  - `int enroll_offer_open(const uint8_t *in, size_t len, const uint8_t ctrl_pk[32], uint8_t vigil_eph_pk_out[32], uint8_t nonce_v_out[24], uint32_t *epoch_out);` → 0/−1.
  - `int enroll_request_build(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t id_sk[32], const uint8_t nonce_d[24], const uint8_t vigil_eph_pk[32], const uint8_t nonce_v[24]);` → `ENROLL_REQUEST_LEN` or −1.
  - `int enroll_request_open(const uint8_t *in, size_t len, const uint8_t vigil_eph_sk[32], uint8_t id_pk_out[32], uint8_t nonce_d_out[24], uint8_t nonce_v_echo_out[24]);` → 0/−1.
  - `int enroll_grant_seal(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t vigil_eph_sk[32], const uint8_t nonce_d[24], const uint8_t k_fleet[32], uint32_t epoch);` → `ENROLL_GRANT_LEN` or −1.
  - `int enroll_grant_open(const uint8_t *in, size_t len, const uint8_t vigil_eph_pk[32], const uint8_t id_sk[32], uint8_t k_fleet_out[32], uint32_t *epoch_out, uint8_t nonce_d_echo_out[24]);` → 0/−1.

- [ ] **Step 1: Write the header**

Create `components/simulacra_radar/enroll_wire.h`:

```c
#pragma once
#include <stdint.h>
#include <stddef.h>

// Raw ESP-NOW enrollment frame types (NOT radar_wire_seal-wrapped).
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

int enroll_offer_sign(uint8_t *out, size_t cap, const uint8_t vigil_eph_pk[32],
                      const uint8_t nonce_v[24], uint32_t epoch, const uint8_t ctrl_sk[64]);
int enroll_offer_open(const uint8_t *in, size_t len, const uint8_t ctrl_pk[32],
                      uint8_t vigil_eph_pk_out[32], uint8_t nonce_v_out[24], uint32_t *epoch_out);

int enroll_request_build(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t id_sk[32],
                         const uint8_t nonce_d[24], const uint8_t vigil_eph_pk[32], const uint8_t nonce_v[24]);
int enroll_request_open(const uint8_t *in, size_t len, const uint8_t vigil_eph_sk[32],
                        uint8_t id_pk_out[32], uint8_t nonce_d_out[24], uint8_t nonce_v_echo_out[24]);

int enroll_grant_seal(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t vigil_eph_sk[32],
                      const uint8_t nonce_d[24], const uint8_t k_fleet[32], uint32_t epoch);
int enroll_grant_open(const uint8_t *in, size_t len, const uint8_t vigil_eph_pk[32], const uint8_t id_sk[32],
                      uint8_t k_fleet_out[32], uint32_t *epoch_out, uint8_t nonce_d_echo_out[24]);
```

- [ ] **Step 2: Write the failing test**

In `main/churn_selftest.c`, add before `churn_selftest_run()` (add `#include "enroll_wire.h"` near the other radar includes):

```c
static void test_enroll_wire(void)
{
    uint8_t ctrl_pk[32], ctrl_sk[64];  crypto_sign_keypair(ctrl_pk, ctrl_sk);      // Vigil signing key
    uint8_t veph_pk[32], veph_sk[32];  crypto_box_keypair(veph_pk, veph_sk);        // Vigil ephemeral
    uint8_t id_pk[32],  id_sk[32];     crypto_box_keypair(id_pk, id_sk);            // decoy identity
    uint8_t nonce_v[24], nonce_d[24];
    for (int i=0;i<24;i++){ nonce_v[i]=(uint8_t)(i+1); nonce_d[i]=(uint8_t)(0x40+i); }
    uint32_t epoch = 7;
    uint8_t k_fleet[32]; for (int i=0;i<32;i++) k_fleet[i]=(uint8_t)(0x80+i);

    // OFFER
    uint8_t offer[ENROLL_OFFER_LEN];
    ST_CHECK(enroll_offer_sign(offer, sizeof offer, veph_pk, nonce_v, epoch, ctrl_sk) == ENROLL_OFFER_LEN,
             "offer signs to expected len");
    uint8_t o_eph[32], o_nv[24]; uint32_t o_ep;
    ST_CHECK(enroll_offer_open(offer, ENROLL_OFFER_LEN, ctrl_pk, o_eph, o_nv, &o_ep) == 0, "offer verifies");
    ST_CHECK(memcmp(o_eph, veph_pk, 32)==0 && memcmp(o_nv, nonce_v, 24)==0 && o_ep==epoch, "offer fields recovered");
    offer[2] ^= 0x01;
    ST_CHECK(enroll_offer_open(offer, ENROLL_OFFER_LEN, ctrl_pk, o_eph, o_nv, &o_ep) != 0, "tampered offer rejected");
    offer[2] ^= 0x01;
    uint8_t wrong_pk[32], wrong_sk[64]; crypto_sign_keypair(wrong_pk, wrong_sk);
    ST_CHECK(enroll_offer_open(offer, ENROLL_OFFER_LEN, wrong_pk, o_eph, o_nv, &o_ep) != 0, "wrong ctrl_pk rejected");

    // REQUEST
    uint8_t req[ENROLL_REQUEST_LEN];
    ST_CHECK(enroll_request_build(req, sizeof req, id_pk, id_sk, nonce_d, veph_pk, nonce_v) == ENROLL_REQUEST_LEN,
             "request builds to expected len");
    uint8_t r_idpk[32], r_nd[24], r_nv[24];
    ST_CHECK(enroll_request_open(req, ENROLL_REQUEST_LEN, veph_sk, r_idpk, r_nd, r_nv) == 0, "request opens");
    ST_CHECK(memcmp(r_idpk, id_pk, 32)==0 && memcmp(r_nd, nonce_d, 24)==0 && memcmp(r_nv, nonce_v, 24)==0,
             "request fields recovered (id_pk, nonce_d, echoed nonce_v)");
    req[40] ^= 0x01;   // flip a ciphertext byte
    ST_CHECK(enroll_request_open(req, ENROLL_REQUEST_LEN, veph_sk, r_idpk, r_nd, r_nv) != 0, "tampered request rejected");
    req[40] ^= 0x01;

    // GRANT
    uint8_t grant[ENROLL_GRANT_LEN];
    ST_CHECK(enroll_grant_seal(grant, sizeof grant, id_pk, veph_sk, nonce_d, k_fleet, epoch) == ENROLL_GRANT_LEN,
             "grant seals to expected len");
    uint8_t g_key[32], g_nd[24]; uint32_t g_ep;
    ST_CHECK(enroll_grant_open(grant, ENROLL_GRANT_LEN, veph_pk, id_sk, g_key, &g_ep, g_nd) == 0, "grant opens");
    ST_CHECK(memcmp(g_key, k_fleet, 32)==0 && g_ep==epoch && memcmp(g_nd, nonce_d, 24)==0, "grant delivers key+epoch+nonce_d");
    uint8_t bad_pk[32], bad_sk[32]; crypto_box_keypair(bad_pk, bad_sk);
    ST_CHECK(enroll_grant_open(grant, ENROLL_GRANT_LEN, veph_pk, bad_sk, g_key, &g_ep, g_nd) != 0, "wrong id_sk cannot open grant");
}
```

Register it: add `test_enroll_wire();` to `churn_selftest_run()` after `test_config_wire();`.

- [ ] **Step 3: Run and verify it fails**

Run: `idf.py -DCHURN_SELFTEST=1 -DSIMULACRA_ESPNOW=1 build`
Expected: **compile error** — `enroll_wire.h` functions undefined / `enroll_wire.c` not yet implemented.

- [ ] **Step 4: Write the implementation**

Create `components/simulacra_radar/enroll_wire.c`:

```c
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
    memcpy(m, TAG_OFFER, 4); memcpy(m+4, eph, 32); memcpy(m+36, nv, 24);
    m[60]=(uint8_t)epoch; m[61]=(uint8_t)(epoch>>8); m[62]=(uint8_t)(epoch>>16); m[63]=(uint8_t)(epoch>>24);
}

int enroll_offer_sign(uint8_t *out, size_t cap, const uint8_t veph[32], const uint8_t nv[24],
                      uint32_t epoch, const uint8_t ctrl_sk[64])
{
    if (cap < ENROLL_OFFER_LEN) return -1;
    uint8_t msg[OFFER_SIGNED_LEN]; offer_signed_msg(msg, veph, nv, epoch);
    uint8_t sm[64 + OFFER_SIGNED_LEN]; unsigned long long smlen = 0;
    if (crypto_sign(sm, &smlen, msg, OFFER_SIGNED_LEN, ctrl_sk) != 0) return -1;
    out[0] = ENROLL_VER; memcpy(out+1, veph, 32); memcpy(out+33, nv, 24);
    memcpy(out+57, msg+60, 4);                  // epoch (4)
    memcpy(out+61, sm, 64);                     // detached sig = first 64 of sm
    return ENROLL_OFFER_LEN;
}

int enroll_offer_open(const uint8_t *in, size_t len, const uint8_t ctrl_pk[32],
                      uint8_t veph_out[32], uint8_t nv_out[24], uint32_t *epoch_out)
{
    if (len != ENROLL_OFFER_LEN || in[0] != ENROLL_VER) return -1;
    uint32_t epoch = (uint32_t)in[57] | ((uint32_t)in[58]<<8) | ((uint32_t)in[59]<<16) | ((uint32_t)in[60]<<24);
    uint8_t msg[OFFER_SIGNED_LEN]; offer_signed_msg(msg, in+1, in+33, epoch);
    uint8_t sm[64 + OFFER_SIGNED_LEN], rec[64 + OFFER_SIGNED_LEN]; unsigned long long rlen = 0;
    memcpy(sm, in+61, 64); memcpy(sm+64, msg, OFFER_SIGNED_LEN);
    if (crypto_sign_open(rec, &rlen, sm, 64 + OFFER_SIGNED_LEN, ctrl_pk) != 0) return -1;
    if (rlen != OFFER_SIGNED_LEN) return -1;
    memcpy(veph_out, in+1, 32); memcpy(nv_out, in+33, 24); *epoch_out = epoch;
    return 0;
}

// ---- REQUEST (box: TAG_REQ|nonce_v, from id_sk to vigil_eph_pk) ----
int enroll_request_build(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t id_sk[32],
                         const uint8_t nonce_d[24], const uint8_t veph[32], const uint8_t nv[24])
{
    if (cap < ENROLL_REQUEST_LEN) return -1;
    uint8_t bn[24]; randombytes(bn, 24);
    uint8_t pt[4 + 24]; memcpy(pt, TAG_REQ, 4); memcpy(pt+4, nv, 24);
    out[0] = ENROLL_VER; memcpy(out+1, id_pk, 32); memcpy(out+33, nonce_d, 24); memcpy(out+57, bn, 24);
    int bl = box_seal(out+81, pt, sizeof pt, bn, veph, id_sk);
    if (bl != 44) return -1;
    return ENROLL_REQUEST_LEN;
}

int enroll_request_open(const uint8_t *in, size_t len, const uint8_t veph_sk[32],
                        uint8_t id_pk_out[32], uint8_t nonce_d_out[24], uint8_t nv_echo_out[24])
{
    if (len != ENROLL_REQUEST_LEN || in[0] != ENROLL_VER) return -1;
    const uint8_t *id_pk = in+1, *bn = in+57, *box = in+81;
    uint8_t pt[4 + 24];
    if (box_open(pt, box, 44, bn, id_pk, veph_sk) != 0) return -1;
    if (memcmp(pt, TAG_REQ, 4) != 0) return -1;
    memcpy(id_pk_out, id_pk, 32); memcpy(nonce_d_out, in+33, 24); memcpy(nv_echo_out, pt+4, 24);
    return 0;
}

// ---- GRANT (box: TAG_GRANT|k_fleet|epoch|nonce_d, from vigil_eph_sk to id_pk) ----
int enroll_grant_seal(uint8_t *out, size_t cap, const uint8_t id_pk[32], const uint8_t veph_sk[32],
                      const uint8_t nonce_d[24], const uint8_t k_fleet[32], uint32_t epoch)
{
    if (cap < ENROLL_GRANT_LEN) return -1;
    uint8_t bn[24]; randombytes(bn, 24);
    uint8_t pt[4 + 32 + 4 + 24];
    memcpy(pt, TAG_GRANT, 4); memcpy(pt+4, k_fleet, 32);
    pt[36]=(uint8_t)epoch; pt[37]=(uint8_t)(epoch>>8); pt[38]=(uint8_t)(epoch>>16); pt[39]=(uint8_t)(epoch>>24);
    memcpy(pt+40, nonce_d, 24);
    out[0] = ENROLL_VER; memcpy(out+1, bn, 24);
    int bl = box_seal(out+25, pt, sizeof pt, bn, id_pk, veph_sk);
    if (bl != 80) return -1;
    return ENROLL_GRANT_LEN;
}

int enroll_grant_open(const uint8_t *in, size_t len, const uint8_t veph[32], const uint8_t id_sk[32],
                      uint8_t k_fleet_out[32], uint32_t *epoch_out, uint8_t nonce_d_echo_out[24])
{
    if (len != ENROLL_GRANT_LEN || in[0] != ENROLL_VER) return -1;
    const uint8_t *bn = in+1, *box = in+25;
    uint8_t pt[4 + 32 + 4 + 24];
    if (box_open(pt, box, 80, bn, veph, id_sk) != 0) return -1;
    if (memcmp(pt, TAG_GRANT, 4) != 0) return -1;
    memcpy(k_fleet_out, pt+4, 32);
    *epoch_out = (uint32_t)pt[36] | ((uint32_t)pt[37]<<8) | ((uint32_t)pt[38]<<16) | ((uint32_t)pt[39]<<24);
    memcpy(nonce_d_echo_out, pt+40, 24);
    return 0;
}
```

Add `enroll_wire.c` to `SRCS` in `components/simulacra_radar/CMakeLists.txt`, and ensure `tweetnacl` is in that component's `REQUIRES`.

- [ ] **Step 5: Run and verify it passes**

Run: `idf.py -DCHURN_SELFTEST=1 -DSIMULACRA_ESPNOW=1 build flash monitor`
Expected: serial shows the new checks passing and the final `selftest: N/N pass` count increased by 12 (the `ST_CHECK`s above) with 0 failures.

- [ ] **Step 6: Commit**

```bash
git add components/simulacra_radar/enroll_wire.h components/simulacra_radar/enroll_wire.c \
        components/simulacra_radar/CMakeLists.txt main/churn_selftest.c
git commit -m "feat(fleet): enrollment wire crypto (signed OFFER, boxed REQUEST/GRANT)"
```

---

### Task 2: Decoy fleet-key store (NVS identity + fleet key)

NVS-backed identity keypair, fleet key, epoch, and previous key; fingerprint for TOFU. Gated by `SIMULACRA_FLEET_PROVISION`.

**Files:**
- Create: `main/fleet_key.h`, `main/fleet_key.c`
- Modify: `main/CMakeLists.txt` (add `fleet_key.c` to `SRCS`; add `SIMULACRA_FLEET_PROVISION` to the `foreach` forwarder)
- Modify: `components/simulacra_radar/radar_key.h` (guard the compile-time key behind the gate being **off**)
- Test: `main/churn_selftest.c` (`test_fleet_key()` — on-target NVS round-trip; mirror `test_learn_nvs`)

**Interfaces:**
- Produces (used by Tasks 3 & 7):
  - `void fleet_key_init(void);` — load-or-create identity keypair in NVS; load fleet key/epoch if present.
  - `bool fleet_key_have(void);` — true once a fleet key is stored.
  - `const uint8_t *fleet_key_get(void);` — current 32-B fleet key (or the compile-time fallback when the gate is off / none stored yet). `NULL` if unkeyed and gate on.
  - `const uint8_t *fleet_key_prev(void);` — previous 32-B key during grace, or `NULL`.
  - `void fleet_key_set(const uint8_t key[32], uint32_t epoch);` — store new key (moves current→prev if epoch advanced), persist to NVS.
  - `const uint8_t *fleet_id_pk(void);` — 32-B identity public key.
  - `void fleet_id_sk(uint8_t out[32]);` — copy identity secret (used only by the enroll responder).
  - `void fleet_id_fingerprint(char *out, size_t cap);` — `"xxxx-xxxx-xxxx-xxxx"` hex of SHA-256(id_pk)[0..7].

- [ ] **Step 1: Write the header** — `main/fleet_key.h` with the signatures above and `#define FLEET_NVS_NS "simfleet"`.

- [ ] **Step 2: Write the failing test**

In `main/churn_selftest.c` add:

```c
static void test_fleet_key(void)
{
    fleet_key_init();
    ST_CHECK(fleet_id_pk() != NULL, "identity pubkey available after init");
    char fp1[24]; fleet_id_fingerprint(fp1, sizeof fp1);
    ST_CHECK(strlen(fp1) == 19, "fingerprint formatted xxxx-xxxx-xxxx-xxxx");

    uint8_t k[32]; for (int i=0;i<32;i++) k[i]=(uint8_t)(i*3+1);
    fleet_key_set(k, 5);
    ST_CHECK(fleet_key_have() && memcmp(fleet_key_get(), k, 32)==0, "fleet key stored + readable");

    uint8_t k2[32]; for (int i=0;i<32;i++) k2[i]=(uint8_t)(i*7+2);
    fleet_key_set(k2, 6);
    ST_CHECK(memcmp(fleet_key_get(), k2, 32)==0, "rotated key becomes current");
    ST_CHECK(fleet_key_prev() && memcmp(fleet_key_prev(), k, 32)==0, "old key retained as prev during grace");
}
```

Register `test_fleet_key();` in `churn_selftest_run()`.

- [ ] **Step 3: Run to verify it fails** — `idf.py -DCHURN_SELFTEST=1 -DSIMULACRA_ESPNOW=1 -DSIMULACRA_FLEET_PROVISION=1 build` → undefined `fleet_key_*`.

- [ ] **Step 4: Implement `main/fleet_key.c`**

Use `crypto_box_keypair` for identity; NVS blob-per-key (`nvs_get_blob`/`nvs_set_blob`) in namespace `simfleet`: `id_sk`(32), `k_fleet`(32), `k_prev`(32), `k_epoch`(u32). On `fleet_key_init`: open NVS, load `id_sk` or generate+persist, derive `id_pk` via `crypto_scalarmult_base`. Keep current/prev/epoch in static RAM mirrors. `fleet_id_fingerprint`: `crypto_hash(digest, id_pk, 32)` (confirmed `crypto_hash` = SHA-512 in the vendored header), take the first 8 bytes, format as `%02x%02x-%02x%02x-%02x%02x-%02x%02x`. Derive `id_pk` from the stored `id_sk` with `crypto_scalarmult_base(id_pk, id_sk)` (confirmed exposed). When `SIMULACRA_FLEET_PROVISION` is **not** defined, `fleet_key_get()` returns `SIMULACRA_ESPNOW_KEY` and `fleet_key_have()` returns true (back-compat).

- [ ] **Step 5: Gate `radar_key.h`** — wrap so that with the gate **on**, `SIMULACRA_ESPNOW_KEY` is still declared (used as fallback constant only where `fleet_key.c` references it), and add `SIMULACRA_FLEET_PROVISION` to the `foreach` list in `main/CMakeLists.txt`. Add `fleet_key.c` to `SRCS`; add `nvs_flash` to `REQUIRES` if not already present (it is).

- [ ] **Step 6: Run to verify it passes** — flash with the three `-D` flags; confirm the 5 new checks pass and persist across a manual reset (re-run: fingerprint identical, key still present).

- [ ] **Step 7: Commit** — `feat(fleet): NVS identity keypair + fleet-key store with rotation grace`.

---

### Task 3: Decoy enrollment responder + seek-mode + try-two-keys

Wire Tasks 1–2 into the live decoy: seek enrollment when unkeyed, run the handshake, and decrypt with current-then-previous key during grace.

**Files:**
- Modify: `main/esp_now_link.c` (handle raw ENROLL frames; seek-enroll loop; `open_with_fleet_key` helper)
- Modify: `main/CMakeLists.txt` (REQUIRES already covers deps)
- Test: `main/churn_selftest.c` (`test_fleet_open_grace()` — a frame sealed under the *previous* key opens; a frame under an unrelated key does not)

**Interfaces:**
- Consumes: `enroll_*` (Task 1), `fleet_key_*` (Task 2), `SIMULACRA_CTRL_PK`, `radar_wire_seal/open`.
- Produces: internal to the decoy; no new public API.

- [ ] **Step 1: Write the failing test** — seal a `RADAR_TYPE_STATUS` frame under `k_prev`, assert a helper `espnow_open_any(frame,len,...)` (added in Step 3) decrypts it by falling back to prev; seal under a random key and assert it fails.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement** — in `on_recv`, replace the single `radar_wire_open(..., SIMULACRA_ESPNOW_KEY, ...)` with a helper that tries `fleet_key_get()` then `fleet_key_prev()`. Add a raw-frame path *before* the wire-open (raw ENROLL types are not sealed): parse the first byte header/type for `RADAR_TYPE_ENROLL_OFFER` and, when `!fleet_key_have()` (or in an operator-less always-listen policy), build+send REQUEST (`enroll_request_build` with `fleet_id_*`), then on `RADAR_TYPE_ENROLL_GRANT` call `enroll_grant_open`; on success `fleet_key_set()` and log. Seek-mode: when `!fleet_key_have()`, `espnow_task` skips telemetry seal calls (can't seal) and just listens. Guard the whole block with `#ifdef SIMULACRA_FLEET_PROVISION`.

- [ ] **Step 4: Run to verify it passes** (selftest), then **Step 5: Commit** — `feat(fleet): decoy enrollment responder + seek-mode + grace-key open`.

---

### Task 4: Vigil fleet DB (allowlist + fleet key, sealed on SD)

Vigil's persistent state: the fleet key/epoch and the decoy identity allowlist, sealed on SD reusing `learn_db`.

**Files:**
- Create: `cyd/main/fleet_db.h`, `cyd/main/fleet_db.c`
- Modify: `cyd/main/CMakeLists.txt` (add source; ensure `simulacra_radar`+`fatfs` in REQUIRES)
- Test: `cyd/main/cyd_selftest` path or a `-DFLEET_SELFTEST` block — seal/open round-trip, allowlist add/contains/remove, key rotate bumps epoch.

**Interfaces:**
- Produces (used by Tasks 5–6):
  - `void fleet_db_load(void);` / `void fleet_db_save(void);` — sealed `/sdcard/simulacra/fleet.db` via `learn_db_seal/open` key schedule (or a parallel `fleet_db_seal` if the record type differs).
  - `const uint8_t *fleet_db_key(void);` / `uint32_t fleet_db_epoch(void);`
  - `void fleet_db_rotate(void);` — new random key, `epoch++`.
  - `bool fleet_allow_contains(const uint8_t id_pk[32]);`
  - `void fleet_allow_add(const uint8_t id_pk[32]);` / `void fleet_allow_remove(const uint8_t id_pk[32]);`
  - `size_t fleet_allow_count(void);` / `const uint8_t *fleet_allow_at(size_t i);`

- [ ] Steps: TDD as above — write seal/open + allowlist tests, run-fail, implement (fixed-capacity allowlist array e.g. `FLEET_ALLOW_CAP 32`; sealed blob = `epoch(4) | key(32) | count(2) | id_pk[count][32]`), run-pass, commit `feat(fleet): Vigil SD-sealed fleet key + identity allowlist`.

---

### Task 5: Vigil enrollment authority (pairing window, OFFER/REQUEST/GRANT)

Operator-gated pairing window; broadcast signed OFFERs; handle REQUEST with allowlist check; send GRANT.

**Files:**
- Modify: `cyd/main/cyd_main.c` (pairing state machine, OFFER broadcaster in the ESP-NOW task, RX handling of `RADAR_TYPE_ENROLL_REQUEST`, GRANT send)
- Consumes: `enroll_*` (Task 1), `fleet_db_*` (Task 4), `SIMULACRA_CTRL_SK`.

- [ ] Steps: add a `s_pairing_until_ms` window (opened in Task 6 by a touch); while open, broadcast `enroll_offer_sign(...)` with a fresh ephemeral keypair + `nonce_v` every ~1 s; on `RADAR_TYPE_ENROLL_REQUEST`, `enroll_request_open` with `vigil_eph_sk`, verify echoed `nonce_v` matches the current window, then: if `fleet_allow_contains(id_pk)` → `enroll_grant_seal` + send; else stage a *pending* `id_pk` for the Task 6 accept UI. Reuse one ephemeral keypair per window. Commit `feat(vigil): enrollment authority + pairing window`.

---

### Task 6: Vigil TOFU accept UI + rotate/revoke actions

Touchscreen: show a pending requester's fingerprint with Accept/Reject; a control to open the pairing window; rotate/revoke actions.

**Files:**
- Modify: `cyd/main/cyd_main.c` (+ radar_ui CONTROL page or a new ENROLL view) — render pending fingerprint, Accept→`fleet_allow_add`+grant, Reject→drop; buttons: "Enroll" (open window), "Rotate" (`fleet_db_rotate` + re-enroll allowlisted), "Revoke" (select id → `fleet_allow_remove` + rotate).

- [ ] Steps: add an ENROLL view to the view cycle; render `fleet_id`-style fingerprint of the pending `id_pk` (reuse the decoy's fingerprint format so operator can string-match the serial print); wire touch zones; persist via `fleet_db_save`. Commit `feat(vigil): TOFU accept UI + rotate/revoke`.

---

### Task 7: On-target bring-up + 2-board soak

**Files:** none (validation) — capture results in the commit message / memory.

- [ ] **Step 1:** Flash Vigil (CYD) with the fleet-DB + authority build; flash one decoy (C5 or C6) with `-DSIMULACRA_ESPNOW=1 -DSIMULACRA_CONFIG_CTRL=1 -DSIMULACRA_FLEET_PROVISION=1` and **no** stored fleet key.
- [ ] **Step 2:** Confirm decoy boots into seek-mode (serial: no fleet key, listening) and prints its identity fingerprint.
- [ ] **Step 3:** Tap **Enroll** on Vigil → decoy's fingerprint appears on-screen → verify it matches the serial print → **Accept**. Confirm decoy logs GRANT + stores key, then telemetry/radar starts flowing (STATUS answered).
- [ ] **Step 4:** Reboot the decoy → it comes up already-keyed (NVS), no re-enroll needed.
- [ ] **Step 5:** **Rotate** on Vigil → decoy re-enrolls to the new epoch; telemetry continues (grace covers in-flight frames).
- [ ] **Step 6:** **Revoke** the decoy + rotate → decoy's frames are no longer accepted (dropped node goes dark). Confirm no crash, main-task stack healthy (`uxTaskGetStackHighWaterMark`), 10-min soak stable.
- [ ] **Step 7:** Update memory ([[feature-backlog]] item closed, [[vigil-remote-control]] cross-link) and open/append the PR.

---

## Self-Review

- **Spec coverage:** enrollment handshake (T1), NVS identity+key (T2), decoy responder/seek/grace (T3), Vigil allowlist+key on SD (T4), pairing authority (T5), TOFU UI + rotate/revoke (T6), bring-up/soak (T7) — every spec section maps to a task.
- **Type consistency:** `enroll_*` signatures in T1 match their call sites in T3/T5; `fleet_key_*` (T2) and `fleet_db_*` (T4) names are used consistently; frame types 8/9/10 verified free.
- **Placeholders:** T1–T2 carry complete code; T3–T7 are integration/UI/bench tasks with concrete function-level steps against named existing code (`on_recv`, `espnow_task`, `cyd_main.c`, `radar_ui`). No `TBD`.
- **Gotchas flagged:** NaCl box zero-padding isolated in `box_seal/open`; detached-sig trick mirrors `config_wire.c`; build-gate forwarder; back-compat fallback key; confirm which SHA TweetNaCl exposes for the fingerprint (T2 Step 4).

## Execution Handoff

Plan saved. Two execution options: **(1) Subagent-driven** (fresh subagent per task, review between) or **(2) Inline** (this session, checkpoints). Tasks 1, 2, 4 are fully desk-testable on target; 3, 5, 6 are integration; 7 needs two physical boards.
