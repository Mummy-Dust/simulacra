#include <string.h>
#include "probe_frame.h"
#include "esp_random.h"
#include "uniq_id.h"

// ============================================================================
// Archetype IE bodies (the frame body after the 24-byte MAC header + seq ctl).
//
// Every tail begins with a WILDCARD SSID (id 0x00, len 0) -- Law 3: we never name
// a network. The DS Parameter Set (id 0x03) carries a placeholder channel that
// probe_build_request() patches per burst. All lengths are self-consistent
// (each id,len pair's len == its data byte count).
//
// source: MODELED from documented iOS/Android probe-request structures. These are
// stand-ins until the capture-driven enrichment milestone replaces them with real
// (structure-only) captures. The host tests (tools/probe_audit) pin them.
// ============================================================================

// --- shared building blocks (documented, band-appropriate) ------------------
// Supported Rates 2.4 GHz: 1/2/5.5/11 (CCK, basic) + 6/9/12/18 (OFDM)
#define RATES_24   0x01,0x08,0x82,0x84,0x8b,0x96,0x0c,0x12,0x18,0x24
#define EXTRATES_24 0x32,0x04,0x30,0x48,0x60,0x6c            // 24/36/48/54
// Supported Rates 5 GHz: 6/9/12/18/24/36/48/54 (OFDM only, no CCK)
#define RATES_5    0x01,0x08,0x8c,0x12,0x98,0x24,0xb0,0x48,0x60,0x6c
#define DS_PARAM   0x03,0x01,0x00                            // channel patched at TX
// HT Capabilities (id 0x2D, 26 bytes) -- two capinfo variants for archetype diversity
#define HT_A  0x2d,0x1a,0xad,0x01,0x17,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00, \
              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
#define HT_B  0x2d,0x1a,0x6f,0x01,0x1b,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00, \
              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
// VHT Capabilities (id 0xBF, 12 bytes) -- 5 GHz only
#define VHT   0xbf,0x0c,0x92,0x71,0x80,0x0f,0xea,0xff,0x00,0x00,0xea,0xff,0x00,0x00
// HE Capabilities (id 0xFF, ext id 0x23) -- present on ax devices, both bands
#define HE    0xff,0x16,0x23,0x09,0x01,0x08,0x1a,0x40,0x00,0x04,0x70,0x0c,0x89,0x7c, \
              0xc8,0x07,0xcc,0xcc,0xcc,0x00,0x00,0x00,0x00,0x00
// Extended Capabilities variants
#define EXTCAP_APPLE   0x7f,0x08,0x04,0x00,0x08,0x84,0x00,0x00,0x00,0x40
#define EXTCAP_B       0x7f,0x0a,0x04,0x00,0x0a,0x82,0x21,0x40,0x00,0x40,0x00,0x20
#define EXTCAP_C       0x7f,0x09,0x05,0x00,0x0a,0x02,0x01,0x00,0x40,0x00,0x40
#define EXTCAP_SHORT   0x7f,0x02,0x00,0x00
// Vendor-specific IEs (capability/type only -- no identity)
#define VS_APPLE       0xdd,0x0a,0x00,0x17,0xf2,0x0a,0x00,0x01,0x04,0x00,0x00,0x00
#define VS_BROADCOM    0xdd,0x07,0x00,0x10,0x18,0x02,0x00,0x00,0x00
#define VS_WMM         0xdd,0x07,0x00,0x50,0xf2,0x02,0x00,0x01,0x00
#define VS_WPS         0xdd,0x09,0x00,0x50,0xf2,0x04,0x10,0x4a,0x00,0x01,0x10

// --- iPhone: HT + HE (+VHT on 5) + Apple vendor -----------------------------
static const uint8_t IPHONE_24[] = { 0x00,0x00, RATES_24, EXTRATES_24, DS_PARAM,
                                     HT_A, EXTCAP_APPLE, HE, VS_APPLE };
static const uint8_t IPHONE_5[]  = { 0x00,0x00, RATES_5, DS_PARAM,
                                     HT_A, VHT, HE, VS_APPLE };
// --- Galaxy: HT(B) + HE (+VHT on 5) + Broadcom + WMM -------------------------
static const uint8_t GALAXY_24[] = { 0x00,0x00, RATES_24, EXTRATES_24, DS_PARAM,
                                     HT_B, EXTCAP_B, HE, VS_BROADCOM, VS_WMM };
static const uint8_t GALAXY_5[]  = { 0x00,0x00, RATES_5, DS_PARAM,
                                     HT_B, VHT, HE, VS_BROADCOM, VS_WMM };
// --- Pixel: HT(B) + HE (+VHT on 5) + WPS/WFA --------------------------------
static const uint8_t PIXEL_24[]  = { 0x00,0x00, RATES_24, EXTRATES_24, DS_PARAM,
                                     HT_B, EXTCAP_C, HE, VS_WPS };
static const uint8_t PIXEL_5[]   = { 0x00,0x00, RATES_5, DS_PARAM,
                                     HT_B, VHT, HE, VS_WPS };
// --- generic Android: HT only on 2.4 (no HE), HT+VHT on 5 -------------------
static const uint8_t ANDROID_24[] = { 0x00,0x00, RATES_24, EXTRATES_24, DS_PARAM,
                                      HT_A, EXTCAP_SHORT, VS_WPS };
static const uint8_t ANDROID_5[]  = { 0x00,0x00, RATES_5, DS_PARAM,
                                      HT_A, VHT, EXTCAP_SHORT, VS_WPS };

static const probe_archetype_t ARCHS[PROBE_ARCH_COUNT] = {
    [ARCH_IPHONE]  = { "iphone",  IPHONE_24,  sizeof IPHONE_24,  IPHONE_5,  sizeof IPHONE_5,  40 },
    [ARCH_GALAXY]  = { "galaxy",  GALAXY_24,  sizeof GALAXY_24,  GALAXY_5,  sizeof GALAXY_5,  25 },
    [ARCH_PIXEL]   = { "pixel",   PIXEL_24,   sizeof PIXEL_24,   PIXEL_5,   sizeof PIXEL_5,   15 },
    [ARCH_ANDROID] = { "android", ANDROID_24, sizeof ANDROID_24, ANDROID_5, sizeof ANDROID_5, 20 },
};

const probe_archetype_t *probe_archetype(probe_arch_t a)
{
    return (a < PROBE_ARCH_COUNT) ? &ARCHS[a] : 0;
}

size_t probe_archetype_count(void) { return PROBE_ARCH_COUNT; }

probe_arch_t probe_pick_archetype(void)
{
    uint32_t total = 0;
    for (size_t i = 0; i < PROBE_ARCH_COUNT; i++) total += ARCHS[i].weight;
    if (!total) return ARCH_IPHONE;
    uint32_t r = esp_random() % total;
    for (size_t i = 0; i < PROBE_ARCH_COUNT; i++) {
        if (r < ARCHS[i].weight) return (probe_arch_t)i;
        r -= ARCHS[i].weight;
    }
    return ARCH_IPHONE;
}

void probe_random_mac(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[0] = (uint8_t)((out[0] & 0xFC) | 0x02);   // locally-administered, unicast
        int zero = 1, ff = 1;
        for (int i = 0; i < 6; i++) { if (out[i]) zero = 0; if (out[i] != 0xff) ff = 0; }
        if (zero || ff) continue;
        if (uniq_try(out)) return;                    // shares the allocator with BLE
    }
}

// Walk the copied IE body and patch the DS Parameter Set (id 0x03) channel byte.
// Robust to archetype layout -- no per-archetype offset bookkeeping.
static void patch_ds_channel(uint8_t *body, uint16_t len, uint8_t ch)
{
    uint16_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = body[i], ln = body[i + 1];
        if (id == 0x03 && ln >= 1) { body[i + 2] = ch; return; }
        i += 2 + ln;
    }
}

int probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                        uint8_t *out, size_t *out_len)
{
    const probe_archetype_t *a = probe_archetype(arch);
    if (!a) return 1;
    const uint8_t *tail = band5 ? a->tail5 : a->tail24;
    uint16_t tlen       = band5 ? a->tail5_len : a->tail24_len;
    if (!tail || tlen == 0) return 2;                 // archetype lacks this band
    if (24u + tlen > PROBE_FRAME_MAX) return 3;

    uint8_t *p = out;
    *p++ = 0x40; *p++ = 0x00;                          // frame control: mgmt/probe-req
    *p++ = 0x00; *p++ = 0x00;                          // duration
    memset(p, 0xff, 6); p += 6;                        // DA broadcast
    memcpy(p, mac, 6); p += 6;                         // SA = our randomized MAC
    memset(p, 0xff, 6); p += 6;                        // BSSID broadcast
    *p++ = 0x00; *p++ = 0x00;                          // seq control (driver overwrites)
    memcpy(p, tail, tlen); p += tlen;                  // archetype IE body
    patch_ds_channel(out + 24, tlen, ch);
    *out_len = (size_t)(p - out);
    return 0;
}
