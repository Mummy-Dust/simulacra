#pragma once
#include <stdint.h>
#include <stddef.h>

#define RADAR_TYPE_CONFIG 7          // Vigil -> all decoys: signed settings preset
#define CONFIG_WIRE_VER   1
#define CONFIG_SIG_LEN    64

typedef struct __attribute__((packed)) {
    uint8_t version;                 // CONFIG_WIRE_VER
    uint8_t preset_id;               // sim_preset_t value (validated on the decoy)
} config_cmd_t;

#define CONFIG_WIRE_PAYLOAD_LEN (sizeof(config_cmd_t) + CONFIG_SIG_LEN)   // 66

// Vigil: build payload = cmd || Ed25519_sig(nonce12 || cmd) with secret key sk[64].
// nonce12 = salt(4) || counter(8 BE) — the SAME nonce radar_wire_seal will use.
// Returns the payload length, or -1 on buffer/sign error.
int config_wire_pack_signed(uint8_t *out, size_t out_cap, const config_cmd_t *cmd,
                            const uint8_t nonce12[12], const uint8_t sk[64]);

// Decoy: verify payload against (nonce12 || cmd) with public key pk[32].
// Returns 0 and fills *cmd_out on a valid signature; -1 on any failure.
int config_wire_open_signed(const uint8_t *pl, size_t pl_len, const uint8_t nonce12[12],
                            const uint8_t pk[32], config_cmd_t *cmd_out);
