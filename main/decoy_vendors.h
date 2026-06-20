// decoy_vendors.h — vendor palette for simulacra decoys.
//
// Each entry pairs a Bluetooth SIG "Company Identifier" with a short, plausible
// product-style name. The company ID is what a scanner reads from the
// manufacturer-specific data field to attribute a device to a vendor — this is
// the legitimate, spec-defined vendor signal, and it is NOT what triggers
// pairing pop-ups.
//
// Deliberately EXCLUDED from the default palette:
//   * Apple      (0x004C) — its "Continuity" payload format triggers AirPods/
//                           AirTag/"Find My" pop-ups on nearby iPhones.
//   * Microsoft  (0x0006) — "Swift Pair" beacons trigger Windows pop-ups.
//   * Google Fast Pair (service data 0xFE2C) — triggers Android pairing sheets.
// A privacy decoy needs realistic *presence*, not pop-up prompts aimed at
// bystanders, so those formats are simply never emitted. See simulacra_main.c.
//
// Extend this list from the official assigned-numbers registry if you want a
// denser, more varied crowd:
//   https://www.bluetooth.com/specifications/assigned-numbers/

#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t    company_id;   // Bluetooth SIG company identifier (little-endian on air)
    const char *name;         // <= 12 chars to stay within the 31-byte adv budget; NULL = no name
} vendor_t;

static const vendor_t VENDORS[] = {
    { 0x0075, "Galaxy Buds" },   // Samsung Electronics
    { 0x00E0, "Pixel Buds"  },   // Google
    { 0x009E, "Bose QC"     },   // Bose Corporation
    { 0x0087, "Garmin"      },   // Garmin International
    { 0x012D, "WF-1000XM5"  },   // Sony Corporation
    { 0x0059, NULL          },   // Nordic Semiconductor (nameless sensor)
    { 0x0075, NULL          },   // Samsung (nameless wearable)
    { 0x0087, "vivosmart"   },   // Garmin (band)
    { 0x00E0, NULL          },   // Google (nameless)
    { 0x012D, NULL          },   // Sony (nameless)
};

#define VENDOR_COUNT (sizeof(VENDORS) / sizeof(VENDORS[0]))
