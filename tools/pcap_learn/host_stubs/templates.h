#pragma once
/* Host stub: ONLY the fmt_family_t enum, with the SAME values as main/templates.h,
   so learn_strip's family classification (and thus shape_hash) is device-accurate. */
#include <stddef.h>
#include <stdint.h>
typedef enum {
    FMT_VENDOR_MFG,      /* 0 */
    FMT_IBEACON,         /* 1 */
    FMT_EDDYSTONE_UID,   /* 2 */
    FMT_EDDYSTONE_URL,   /* 3 */
    FMT_SVC_TRACKER,     /* 4 */
} fmt_family_t;
