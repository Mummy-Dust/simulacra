#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RADAR_MAGIC0 0x5A
#define RADAR_MAGIC1 0x4D
#define RADAR_WIRE_VER 1
#define RADAR_TYPE_REQUEST 1
#define RADAR_TYPE_STATUS  2
#define RADAR_MAX_THREATS  8        // must match DETECT_MAX_THREATS
// Threat kind (shared so the renderer can label without depending on detect.h):
#define DETECT_KIND_FOLLOWER 0      // behavioral follower (persistence across epochs)
#define DETECT_KIND_KNOWN    1      // fingerprint match (known device class)
#define RADAR_KEY_LEN   32
#define RADAR_NONCE_LEN 12
#define RADAR_TAG_LEN   16
#define RADAR_HDR_LEN    4          // magic(2)+ver+type
#define RADAR_FRAME_MAX 250

typedef struct __attribute__((packed)) {
    uint32_t uptime_s; uint8_t flags;            // bit0 paused, bit1 config_mode
    uint16_t active_devices, roster_size; uint32_t probes_sent;
    uint16_t epoch, pop_ewma; uint32_t total_obs;
    uint8_t active_target, threat_count;
    struct __attribute__((packed)) {
        uint32_t hash; uint16_t vendor; uint8_t epochs; int8_t best_rssi;
        uint16_t first_epoch, last_epoch;
        uint8_t kind, class_id, category, confidence;   // KNOWN-device fields (kind=DETECT_KIND_*)
        uint8_t sessions_seen, places_seen;             // recurrence counters (escalation)
    } threats[RADAR_MAX_THREATS];
    uint8_t form_restless, form_wandering, form_bound;  // BLE shade-form counts: RPA/NRPA/static
    uint16_t battery_mv;                                 // cell voltage, 0 = no battery / no gauge
    uint8_t  battery_pct;                                // state-of-charge %, 0xFF = unavailable (ADC backend)
} radar_wire_status_t;

typedef struct { uint8_t salt[4]; uint64_t counter; bool seen; } radar_replay_t;

// Build [magic|ver|type|nonce|ct|tag] into frame. nonce = salt(4)|counter(8 BE). magic|ver|type
// authenticated as AAD. Returns 0 on success, <0 on error; *frame_len set to total bytes.
int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[32], const uint8_t salt[4], uint64_t counter);

// Verify + decrypt a frame. Returns 0 on success (type/payload/salt/counter filled), <0 if the
// header/magic/tag is bad. payload buffer must hold >= (frame_len - overhead) bytes.
int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32],
                    uint8_t *out_type, uint8_t *payload, size_t *payload_len,
                    uint8_t out_salt[4], uint64_t *out_counter);

// Replay gate: accept iff salt changed (peer reboot) or counter strictly newer. Updates st.
bool radar_replay_ok(radar_replay_t *st, const uint8_t salt[4], uint64_t counter);
