#include "config_wire.h"
#include "tweetnacl.h"
#include <string.h>

// Signed message m = nonce12(12) || cmd(2)  (14 bytes)
#define CW_MSG_LEN (12 + sizeof(config_cmd_t))

int config_wire_pack_signed(uint8_t *out, size_t out_cap, const config_cmd_t *cmd,
                            const uint8_t nonce12[12], const uint8_t sk[64])
{
    if (out_cap < CONFIG_WIRE_PAYLOAD_LEN) return -1;
    uint8_t m[CW_MSG_LEN];
    memcpy(m, nonce12, 12);
    memcpy(m + 12, cmd, sizeof *cmd);
    uint8_t sm[CONFIG_SIG_LEN + CW_MSG_LEN];            // crypto_sign output = sig || m
    unsigned long long smlen = 0;
    if (crypto_sign(sm, &smlen, m, CW_MSG_LEN, sk) != 0) return -1;
    memcpy(out, cmd, sizeof *cmd);                      // payload = cmd || sig
    memcpy(out + sizeof *cmd, sm, CONFIG_SIG_LEN);      // sig is the first 64 bytes of sm
    return (int)CONFIG_WIRE_PAYLOAD_LEN;
}

int config_wire_open_signed(const uint8_t *pl, size_t pl_len, const uint8_t nonce12[12],
                            const uint8_t pk[32], config_cmd_t *cmd_out)
{
    if (pl_len != CONFIG_WIRE_PAYLOAD_LEN) return -1;
    config_cmd_t cmd; memcpy(&cmd, pl, sizeof cmd);
    uint8_t m[CW_MSG_LEN];
    memcpy(m, nonce12, 12);
    memcpy(m + 12, &cmd, sizeof cmd);
    uint8_t sm[CONFIG_SIG_LEN + CW_MSG_LEN];            // reconstruct sig || m
    memcpy(sm, pl + sizeof cmd, CONFIG_SIG_LEN);
    memcpy(sm + CONFIG_SIG_LEN, m, CW_MSG_LEN);
    uint8_t out[CONFIG_SIG_LEN + CW_MSG_LEN];           // crypto_sign_open writes the recovered m
    unsigned long long outlen = 0;
    if (crypto_sign_open(out, &outlen, sm, CONFIG_SIG_LEN + CW_MSG_LEN, pk) != 0) return -1;
    if (outlen != CW_MSG_LEN) return -1;
    *cmd_out = cmd;
    return 0;
}
