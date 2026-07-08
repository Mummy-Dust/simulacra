#include "fleet_key.h"
#include "tweetnacl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>

#ifndef SIMULACRA_FLEET_PROVISION
#include "radar_key.h"     // SIMULACRA_ESPNOW_KEY — compile-time fallback when the gate is off
#endif

static uint8_t  s_id_sk[32], s_id_pk[32];
static uint8_t  s_key[32], s_prev[32];
static uint32_t s_epoch;
static bool     s_have, s_have_prev;

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(FLEET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;   // best-effort
    nvs_set_blob(h, "id_sk", s_id_sk, 32);
    if (s_have)      nvs_set_blob(h, "k_fleet", s_key, 32);
    if (s_have_prev) nvs_set_blob(h, "k_prev", s_prev, 32);
    nvs_set_u32(h, "k_epoch", s_epoch);
    nvs_commit(h); nvs_close(h);
}

void fleet_key_init(void)
{
    nvs_handle_t h; size_t len; bool have_id = false;
    if (nvs_open(FLEET_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        len = 32; if (nvs_get_blob(h, "id_sk", s_id_sk, &len) == ESP_OK && len == 32) have_id = true;
        len = 32; if (nvs_get_blob(h, "k_fleet", s_key,  &len) == ESP_OK && len == 32) s_have = true;
        len = 32; if (nvs_get_blob(h, "k_prev",  s_prev, &len) == ESP_OK && len == 32) s_have_prev = true;
        nvs_get_u32(h, "k_epoch", &s_epoch);
        nvs_close(h);
    }
    if (have_id) {
        crypto_scalarmult_base(s_id_pk, s_id_sk);   // derive pubkey from stored secret
    } else {
        crypto_box_keypair(s_id_pk, s_id_sk);       // fresh identity (uses randombytes)
        persist();                                  // save id_sk immediately
    }
}

bool fleet_key_have(void)
{
#ifdef SIMULACRA_FLEET_PROVISION
    return s_have;
#else
    return true;
#endif
}

const uint8_t *fleet_key_get(void)
{
#ifdef SIMULACRA_FLEET_PROVISION
    return s_have ? s_key : NULL;
#else
    return SIMULACRA_ESPNOW_KEY;
#endif
}

const uint8_t *fleet_key_prev(void)
{
    return s_have_prev ? s_prev : NULL;
}

void fleet_key_set(const uint8_t key[32], uint32_t epoch)
{
    if (s_have) { memcpy(s_prev, s_key, 32); s_have_prev = true; }   // retire current -> prev
    memcpy(s_key, key, 32); s_epoch = epoch; s_have = true;
    persist();
}

uint32_t fleet_key_epoch(void) { return s_epoch; }

const uint8_t *fleet_id_pk(void) { return s_id_pk; }
void fleet_id_sk(uint8_t out[32]) { memcpy(out, s_id_sk, 32); }

void fleet_id_fingerprint(char *out, size_t cap)
{
    if (cap < 20) { if (cap) out[0] = 0; return; }
    uint8_t d[crypto_hash_BYTES];               // SHA-512
    crypto_hash(d, s_id_pk, 32);
    snprintf(out, cap, "%02x%02x-%02x%02x-%02x%02x-%02x%02x",
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}
