#include <stddef.h>
#include "esp_random.h"

// TweetNaCl's PRNG hook. On-device we never generate keys — crypto_sign_keypair
// is unused; the Ed25519 control keypair is provisioned off-device by
// tools/gen_ctrl_key.py, and both signing and verifying are deterministic. This
// definition exists only to satisfy the linker, but we back it with the ESP32
// hardware RNG so it stays safe if ever called.
void randombytes(unsigned char *p, unsigned long long n)
{
    esp_fill_random(p, (size_t)n);
}
