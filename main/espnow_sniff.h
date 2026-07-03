#pragma once

// Task 9 opsec verifier (SIMULACRA_ESPNOW_SNIFF). Parks a spare board on channel 1 in
// promiscuous mode and decodes the radar ESP-NOW link's frames WITHOUT the key: logs each
// REQUEST/STATUS with its 802.11 source MAC (+ whether it is locally-administered) and a
// ciphertext sample, plus running counts so "decoy stays silent until the CYD asks" is
// observable. Wi-Fi-only; NimBLE never starts. Flash to e.g. the SparkFun C6.
void espnow_sniff_start(void);
