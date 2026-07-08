#!/usr/bin/env python3
"""Generate an Ed25519 control keypair for the Vigil->decoy CONFIG link.
Emits a public-key header for decoys and a secret-key header for Vigil (TweetNaCl 64-byte sk).
Requires the 'cryptography' package (already present in the ESP-IDF Python env)."""
import textwrap
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

def c_array(b):
    hexs = ", ".join(f"0x{x:02x}" for x in b)
    return "\n".join("    " + line for line in textwrap.wrap(hexs, 96))

priv = Ed25519PrivateKey.generate()
seed = priv.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                          serialization.NoEncryption())                       # 32-byte seed
pub  = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)  # 32
sk   = seed + pub                                                             # TweetNaCl sk = seed||pub (64)

with open("components/simulacra_radar/sim_ctrl_key.h", "w") as f:
    f.write("#pragma once\n#include <stdint.h>\n"
            "// Ed25519 PUBLIC key for the Vigil->decoy CONFIG link. Non-secret placeholder;\n"
            "// regenerate with tools/gen_ctrl_key.py before real use. Decoys verify with this.\n"
            f"static const uint8_t SIMULACRA_CTRL_PK[32] = {{\n{c_array(pub)}\n}};\n")

with open("cyd/main/sim_ctrl_sk.h", "w") as f:
    f.write("#pragma once\n#include <stdint.h>\n"
            "// Ed25519 SECRET key (seed||pub) for Vigil. CHANGE ME before real use; keep off the\n"
            "// telemetry-only nodes. TweetNaCl 64-byte secret-key format.\n"
            f"static const uint8_t SIMULACRA_CTRL_SK[64] = {{\n{c_array(sk)}\n}};\n")

print("wrote components/simulacra_radar/sim_ctrl_key.h and cyd/main/sim_ctrl_sk.h")
