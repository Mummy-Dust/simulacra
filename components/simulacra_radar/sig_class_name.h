#pragma once
#include "threat_sig.h"
static inline const char *sig_class_name(uint8_t class_id)
{
    switch (class_id) {
        case SIG_CLASS_AIRTAG:   return "AirTag";
        case SIG_CLASS_SMARTTAG: return "SmartTag";
        case SIG_CLASS_TILE:     return "Tile";
        default:                 return "?";
    }
}
