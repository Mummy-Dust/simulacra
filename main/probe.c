#include <string.h>
#include "probe.h"
#include "esp_random.h"

void probe_random_mac(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)(esp_random() & 0xff);
        out[0] = (uint8_t)((out[0] & 0xFC) | 0x02);   // locally-administered, unicast
        int zero = 1, ff = 1;
        for (int i = 0; i < 6; i++) { if (out[i]) zero = 0; if (out[i] != 0xff) ff = 0; }
        if (!zero && !ff) return;
    }
}

int probe_build_request(const uint8_t mac[6], uint8_t channel, uint8_t *out, size_t *out_len)
{
    static const uint8_t hdr_tail[] = {
        0x00, 0x00,                         // SSID IE: id 0, len 0  (WILDCARD -- Law 3)
        0x01, 0x04, 0x02, 0x04, 0x0b, 0x16, // Supported Rates: 1,2,5.5,11 Mbps
        0x32, 0x04, 0x0c, 0x12, 0x18, 0x24, // Extended Supported Rates: 6,9,12,18 Mbps
        0x03, 0x01, 0x00,                   // DS Parameter Set: id 3, len 1, channel (filled below)
    };
    uint8_t *p = out;
    *p++ = 0x40; *p++ = 0x00;               // frame control: mgmt/probe-req, no flags
    *p++ = 0x00; *p++ = 0x00;               // duration
    memset(p, 0xff, 6); p += 6;             // DA broadcast
    memcpy(p, mac, 6); p += 6;              // SA = our randomized MAC
    memset(p, 0xff, 6); p += 6;             // BSSID broadcast
    *p++ = 0x00; *p++ = 0x00;               // seq control (driver overwrites when en_sys_seq=true)
    memcpy(p, hdr_tail, sizeof(hdr_tail)); p += sizeof(hdr_tail);
    out[p - out - 1] = channel;             // DS param channel = last byte
    *out_len = (size_t)(p - out);
    return 0;
}
