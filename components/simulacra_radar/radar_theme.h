#pragma once
#include <stdint.h>

// RGB888 -> RGB565 (panel native). Constant-foldable so these are compile-time literals.
#define RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8) | (((g)&0xFC)<<3) | ((b)>>3)))

// Necromancer palette (see the design mockup). Named, never inline literals.
#define COL_VOID     RGB565(0x09,0x06,0x0F)   // ground
#define COL_CRYPT    RGB565(0x16,0x0E,0x24)   // panels / tiles
#define COL_EDGE     RGB565(0x2C,0x1E,0x45)   // hairline rules / rune borders
#define COL_BONE     RGB565(0xEC,0xE5,0xD4)   // primary text
#define COL_ASH      RGB565(0x85,0x79,0xA0)   // muted labels
#define COL_ARCANE   RGB565(0xA4,0x5C,0xF5)   // accent — sigils / selection
#define COL_CHANNEL  RGB565(0x4F,0xE0,0xB0)   // semantic: alive
#define COL_WARD     RGB565(0xE6,0xA6,0x4F)   // semantic: warning / dormant
#define COL_HUNTER   RGB565(0xF0,0x55,0x5F)   // semantic: detection

typedef enum {
    SIGIL_CIRCLE = 0,   // the fleet
    SIGIL_HUNTER,       // watching eye
    SIGIL_LIVING,       // heartbeat
    SIGIL_RITE,         // star
    SIGIL_WARD,         // key-eye
    SIGIL_GRIMOIRE,     // tome
    SIGIL_COUNT
} sigil_id_t;
