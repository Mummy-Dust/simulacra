#include <stdbool.h>
#include "churn_selftest.h"
#include "roster.h"
#include "esp_log.h"

static const char *TAG = "selftest";
static int s_total, s_fail;
static const char *s_first_fail;

#define ST_CHECK(cond, msg) do { s_total++; if (!(cond)) { \
    s_fail++; if (!s_first_fail) s_first_fail = (msg); \
    ESP_LOGE(TAG, "FAIL: %s", (msg)); } } while (0)

int churn_selftest_run(void)
{
    s_total = 0; s_fail = 0; s_first_fail = NULL;

    // --- roster ---
    roster_init();
    ST_CHECK(roster_count_in_state(ID_IDLE) == CHURN_ROSTER_SIZE,
             "roster_init: all identities IDLE");
    bool macs_ok = true, payload_ok = true;
    for (size_t i = 0; i < CHURN_ROSTER_SIZE; i++) {
        identity_t *id = roster_at(i);
        if ((id->addr[5] & 0xc0) != 0xc0) macs_ok = false;     // random-static
        if (id->payload_len == 0) payload_ok = false;
    }
    ST_CHECK(macs_ok, "roster: every MAC is random-static (top 2 bits set)");
    ST_CHECK(payload_ok, "roster: every identity has a non-empty payload");

    identity_t *c = roster_promote_candidate(0);
    ST_CHECK(c != NULL, "promote_candidate returns an identity");

    ESP_LOGW(TAG, "SELFTEST: %s (%d/%d)", s_fail ? "FAIL" : "PASS",
             s_total - s_fail, s_total);
    return s_fail;
}
