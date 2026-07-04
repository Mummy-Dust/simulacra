#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SIG_PAT_MAX 16
#define SIG_DB_CAP  64          // RAM working set on both roles

typedef enum { SIG_CAT_TRACKER = 0, SIG_CAT_CAMERA, SIG_CAT_BODYCAM, SIG_CAT_UNKNOWN, SIG_CAT_COUNT } sig_category_t;
typedef enum { SIG_CLASS_AIRTAG = 0, SIG_CLASS_SMARTTAG, SIG_CLASS_TILE, SIG_CLASS_COUNT } sig_class_t;
typedef enum { SIG_SRC_MFG_DATA = 0, SIG_SRC_SVC_DATA, SIG_SRC_COUNT } sig_src_t;

#define SIG_ADDR_PUBLIC  0x01
#define SIG_ADDR_STATIC  0x02   // random static
#define SIG_ADDR_RPA     0x04   // resolvable private (rotating)
#define SIG_ADDR_NRPA    0x08   // non-resolvable private

typedef struct __attribute__((packed)) {
    uint16_t sig_id;
    uint8_t  category;          // sig_category_t
    uint8_t  class_id;          // sig_class_t
    uint16_t company_id;        // required mfg company id; 0xFFFF = don't care
    uint16_t svc_uuid16;        // required 16-bit service UUID; 0x0000 = don't care
    uint8_t  addr_type_mask;    // SIG_ADDR_* bitmask; 0 = any
    uint8_t  match_src;         // sig_src_t
    uint8_t  pat_off;           // offset into that AD payload
    uint8_t  pat_len;           // pattern length (<= SIG_PAT_MAX)
    uint8_t  pattern[SIG_PAT_MAX];
    uint8_t  mask[SIG_PAT_MAX];
    uint8_t  confidence;        // 0..100
} threat_sig_t;

typedef struct {
    uint16_t company_id;        // 0xFFFF if no mfg data
    uint16_t svc_uuid16;        // 0x0000 if none
    uint8_t  addr_type;         // one SIG_ADDR_* bit
    const uint8_t *mfg_data; uint8_t mfg_len;   // includes the 2-byte company id
    const uint8_t *svc_data; uint8_t svc_len;   // includes the 2-byte service uuid
} sig_adv_fields_t;

typedef struct { uint16_t sig_id; uint8_t category; uint8_t class_id; uint8_t confidence; } sig_hit_t;
